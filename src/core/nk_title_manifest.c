/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_title_manifest.h"
#include "nk_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#define MAX_MANIFEST_BYTES (256 * 1024)
#define MAX_MODULES 32
#define MAX_COMPAT_DISC_IDS 16

/* In-memory storage for the registered overlay entry */
static struct {
    char id[64];
    char display_name[128];
    NkTitleKind kind;
    char primary_disc_id[16];
    char compat_ids_storage[MAX_COMPAT_DISC_IDS][16];
    const char *compat_id_ptrs[MAX_COMPAT_DISC_IDS + 1];
    uint32_t executable_base;
    uint32_t executable_entry;
    char data_root[256];
    char memory_stick_root[128];
    char hle_profile[64];
    char codegen_profile[64];
    uint32_t expected_data_file_count;
    bool requires_font_firmware;
    bool requires_psmf;
    NkModuleDefinition modules[MAX_MODULES];
    char module_names[MAX_MODULES][64];
    int module_count;
    NkTitleEntry entry;
} s_overlay_storage;

static FILE *manifest_fopen(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wpath[32768];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 32768);
    return _wfopen(wpath, L"rb");
#else
    return fopen(path, "rb");
#endif
}

static const char *skip_ws(const char *p) {
    if (!p) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *parse_string_literal(const char *p, char *out, size_t max_len) {
    if (!p || *p != '\"') return NULL;
    p++;
    size_t written = 0;
    while (*p && *p != '\"') {
        if ((unsigned char)*p < 0x20) {
            /* Unescaped control characters are invalid in JSON string literals */
            return NULL;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return NULL;
            char c = *p;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == '\"') c = '\"';
            else if (c == '\\') c = '\\';
            else if (c == '/') c = '/';
            if (written + 1 < max_len) {
                out[written++] = c;
            }
        } else {
            if (written + 1 < max_len) {
                out[written++] = *p;
            }
        }
        p++;
    }
    if (*p != '\"') return NULL;
    out[written] = '\0';
    return p + 1;
}

static const char *find_json_key(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    p = skip_ws(p);
    if (!p || *p != ':') return NULL;
    return skip_ws(p + 1);
}

bool nk_title_manifest_load_overlay(
    const char *manifest_path,
    char *error_buf,
    size_t error_buf_len
) {
    if (error_buf && error_buf_len > 0) {
        error_buf[0] = '\0';
    }

    if (!manifest_path || !*manifest_path) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Manifest path is NULL or empty");
        return false;
    }

    FILE *f = manifest_fopen(manifest_path);
    if (!f) {
        if (error_buf) snprintf(error_buf, error_buf_len, "Could not open manifest file: %s", manifest_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > MAX_MANIFEST_BYTES) {
        fclose(f);
        if (error_buf) snprintf(error_buf, error_buf_len, "Manifest size invalid (%ld bytes, limit %d bytes)", sz, MAX_MANIFEST_BYTES);
        return false;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        if (error_buf) snprintf(error_buf, error_buf_len, "Out of memory allocating manifest buffer");
        return false;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read_bytes] = '\0';

    /* 1. Verify schema_version == 1 */
    const char *sv_pos = find_json_key(buf, "schema_version");
    if (!sv_pos) {
        free(buf);
        if (error_buf) snprintf(error_buf, error_buf_len, "Missing required 'schema_version' field");
        return false;
    }
    char *endptr = NULL;
    long schema_version = strtol(sv_pos, &endptr, 10);
    if (endptr == sv_pos || schema_version != 1) {
        free(buf);
        if (error_buf) snprintf(error_buf, error_buf_len, "Unsupported schema_version %ld (must be 1)", schema_version);
        return false;
    }

    memset(&s_overlay_storage, 0, sizeof(s_overlay_storage));

    /* 2. Parse title id */
    const char *id_pos = find_json_key(buf, "id");
    if (!id_pos || !parse_string_literal(id_pos, s_overlay_storage.id, sizeof(s_overlay_storage.id))) {
        free(buf);
        if (error_buf) snprintf(error_buf, error_buf_len, "Missing or invalid required 'id' string");
        return false;
    }

    /* 3. Parse display_name */
    const char *dn_pos = find_json_key(buf, "display_name");
    if (dn_pos) {
        parse_string_literal(dn_pos, s_overlay_storage.display_name, sizeof(s_overlay_storage.display_name));
    }
    if (s_overlay_storage.display_name[0] == '\0') {
        snprintf(s_overlay_storage.display_name, sizeof(s_overlay_storage.display_name), "%s", s_overlay_storage.id);
    }

    /* 4. Parse kind */
    const char *kind_pos = find_json_key(buf, "kind");
    s_overlay_storage.kind = NK_TITLE_KIND_RETAIL;
    if (kind_pos) {
        char kind_str[32] = {0};
        if (parse_string_literal(kind_pos, kind_str, sizeof(kind_str))) {
            if (strcmp(kind_str, "synthetic") == 0) s_overlay_storage.kind = NK_TITLE_KIND_SYNTHETIC;
            else if (strcmp(kind_str, "homebrew") == 0) s_overlay_storage.kind = NK_TITLE_KIND_HOMEBREW;
        }
    }

    /* 5. Parse disc section */
    const char *disc_obj = find_json_key(buf, "disc");
    if (!disc_obj || *disc_obj != '{') {
        free(buf);
        if (error_buf) snprintf(error_buf, error_buf_len, "Missing required 'disc' object");
        return false;
    }
    const char *disc_id_pos = find_json_key(disc_obj, "id");
    if (!disc_id_pos || !parse_string_literal(disc_id_pos, s_overlay_storage.primary_disc_id, sizeof(s_overlay_storage.primary_disc_id))) {
        free(buf);
        if (error_buf) snprintf(error_buf, error_buf_len, "Missing required 'disc.id' string");
        return false;
    }

    /* Optional compatible_disc_ids */
    const char *compat_arr = find_json_key(disc_obj, "compatible_disc_ids");
    int compat_count = 0;
    if (compat_arr && *compat_arr == '[') {
        const char *cp = compat_arr + 1;
        while (cp && *cp && *cp != ']' && compat_count < MAX_COMPAT_DISC_IDS) {
            cp = skip_ws(cp);
            if (*cp == '\"') {
                const char *next = parse_string_literal(cp, s_overlay_storage.compat_ids_storage[compat_count], sizeof(s_overlay_storage.compat_ids_storage[0]));
                if (next) {
                    s_overlay_storage.compat_id_ptrs[compat_count] = s_overlay_storage.compat_ids_storage[compat_count];
                    compat_count++;
                    cp = next;
                } else {
                    break;
                }
            } else if (*cp == ',') {
                cp++;
            } else {
                break;
            }
        }
    }
    s_overlay_storage.compat_id_ptrs[compat_count] = NULL;

    /* 6. Parse filesystem section */
    const char *fs_obj = find_json_key(buf, "filesystem");
    if (fs_obj && *fs_obj == '{') {
        const char *dr_pos = find_json_key(fs_obj, "data_root");
        if (dr_pos) parse_string_literal(dr_pos, s_overlay_storage.data_root, sizeof(s_overlay_storage.data_root));
        const char *ms_pos = find_json_key(fs_obj, "memory_stick_root");
        if (ms_pos) parse_string_literal(ms_pos, s_overlay_storage.memory_stick_root, sizeof(s_overlay_storage.memory_stick_root));
    }

    /* 7. Parse profiles */
    const char *hle_pos = find_json_key(buf, "hle_profile");
    if (hle_pos) parse_string_literal(hle_pos, s_overlay_storage.hle_profile, sizeof(s_overlay_storage.hle_profile));
    const char *cg_pos = find_json_key(buf, "codegen_profile");
    if (cg_pos) parse_string_literal(cg_pos, s_overlay_storage.codegen_profile, sizeof(s_overlay_storage.codegen_profile));

    /* 8. Parse executable section */
    const char *exec_obj = find_json_key(buf, "executable");
    if (exec_obj && *exec_obj == '{') {
        const char *base_pos = find_json_key(exec_obj, "base");
        if (base_pos) s_overlay_storage.executable_base = (uint32_t)strtoul(base_pos, NULL, 0);
        const char *entry_pos = find_json_key(exec_obj, "entry");
        if (entry_pos) s_overlay_storage.executable_entry = (uint32_t)strtoul(entry_pos, NULL, 0);
    }

    /* 9. Parse runtime bindings */
    const char *rb_obj = find_json_key(buf, "runtime_bindings");
    if (rb_obj && *rb_obj == '{') {
        const char *fc_pos = find_json_key(rb_obj, "expected_data_file_count");
        if (fc_pos) {
            s_overlay_storage.expected_data_file_count = (uint32_t)strtoul(fc_pos, NULL, 10);
        }
        const char *fb_pos = find_json_key(rb_obj, "fallback_entry");
        if (fb_pos) {
            uint32_t fb_entry = (uint32_t)strtoul(fb_pos, NULL, 0);
            if (s_overlay_storage.executable_entry == 0 && fb_entry != 0) {
                s_overlay_storage.executable_entry = fb_entry;
            }
        }
    }

    /* 9. Parse modules */
    const char *mod_arr = find_json_key(buf, "modules");
    if (mod_arr && *mod_arr == '[') {
        const char *mp = mod_arr + 1;
        while (mp && *mp && *mp != ']' && s_overlay_storage.module_count < MAX_MODULES) {
            mp = skip_ws(mp);
            if (*mp == '{') {
                int idx = s_overlay_storage.module_count;
                const char *name_pos = find_json_key(mp, "name");
                if (name_pos) {
                    parse_string_literal(name_pos, s_overlay_storage.module_names[idx], sizeof(s_overlay_storage.module_names[idx]));
                    s_overlay_storage.modules[idx].name = s_overlay_storage.module_names[idx];
                }
                const char *addr_pos = find_json_key(mp, "load_address");
                if (addr_pos) {
                    s_overlay_storage.modules[idx].load_address = (uint32_t)strtoul(addr_pos, NULL, 10);
                }
                const char *req_pos = find_json_key(mp, "required");
                if (req_pos && strncmp(req_pos, "true", 4) == 0) {
                    s_overlay_storage.modules[idx].required = true;
                }
                s_overlay_storage.module_count++;
                /* Advance to next module object */
                const char *close_brace = strchr(mp, '}');
                if (close_brace) mp = close_brace + 1;
                else break;
            } else if (*mp == ',') {
                mp++;
            } else {
                break;
            }
        }
    }

    /* Construct NkTitleEntry */
    NkTitleEntry *e = &s_overlay_storage.entry;
    e->id = s_overlay_storage.id;
    e->display_name = s_overlay_storage.display_name;
    e->kind = s_overlay_storage.kind;
    e->primary_disc_id = s_overlay_storage.primary_disc_id;
    e->compatible_disc_ids = s_overlay_storage.compat_id_ptrs;
    e->executable_base = s_overlay_storage.executable_base;
    e->executable_entry = s_overlay_storage.executable_entry;
    e->data_root = s_overlay_storage.data_root[0] ? s_overlay_storage.data_root : NULL;
    e->memory_stick_root = s_overlay_storage.memory_stick_root[0] ? s_overlay_storage.memory_stick_root : NULL;
    e->hle_profile = s_overlay_storage.hle_profile[0] ? s_overlay_storage.hle_profile : NULL;
    e->codegen_profile = s_overlay_storage.codegen_profile[0] ? s_overlay_storage.codegen_profile : NULL;
    e->expected_data_file_count = s_overlay_storage.expected_data_file_count;
    e->modules = s_overlay_storage.modules;
    e->module_count = s_overlay_storage.module_count;

    /* Register overlay in-memory */
    nk_title_catalog_register_overlay(e);

    free(buf);
    return true;
}
