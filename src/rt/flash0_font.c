/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#include "flash0_font.h"

#include <stdlib.h>
#include <string.h>

#include "../core/nk_platform.h"

/* PSP open flags (PSPSDK sceIo): WRONLY=0x0002 (RDWR also sets it), APPEND=0x0100,
 * CREAT=0x0200, TRUNC=0x0400. Any of them is a write intent. */
#define FLASH0_WRITE_INTENT_FLAGS 0x0702u

/* PENDING MEASUREMENT. One entry per slot, indexed by NkFontSlot. served_name is the
 * flash0:/font/ name, and it is also the cache and project file name (nk_font_slots.h).
 * Every entry is pending (measured 0) until the console measurement fixes the lookup (FONT_PLAN
 * M4) and the slot order (M2). A pending entry fails closed: it is not listed and no open of it
 * succeeds. The only writer of measured is the selftest seam. */
typedef struct {
    const char *served_name;
    int measured;
} Flash0SlotEntry;

static Flash0SlotEntry s_flash0_slots[NK_FONT_SLOT_COUNT] = {
    [NK_FONT_SLOT_JAPANESE] = { NK_FONT_SLOT_JAPANESE_FILE, 0 },
    [NK_FONT_SLOT_LATIN]    = { NK_FONT_SLOT_LATIN_FILE, 0 },
    [NK_FONT_SLOT_KOREAN]   = { NK_FONT_SLOT_KOREAN_FILE, 0 },
};

typedef enum {
    FLASH0_PATH_FONT_DIR,   /* flash0:/font or flash0:/font/ */
    FLASH0_PATH_FONT_FILE,  /* flash0:/font/<one name> */
    FLASH0_PATH_OUTSIDE,    /* anywhere else on the device */
    FLASH0_PATH_TRAVERSAL,  /* contains ".." */
    FLASH0_PATH_NOT_A_NAME  /* under /font/ but not a single file name */
} Flash0PathKind;

typedef enum {
    FLASH0_SOURCE_NONE,
    FLASH0_SOURCE_USER,
    FLASH0_SOURCE_PROJECT
} Flash0Source;

static const char *flash0_slot_label(NkFontSlot slot) {
    switch (slot) {
    case NK_FONT_SLOT_JAPANESE: return "japanese";
    case NK_FONT_SLOT_LATIN:    return "latin";
    case NK_FONT_SLOT_KOREAN:   return "korean";
    default:                    return "unknown";
    }
}

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ascii_ci_equal(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = ascii_lower((unsigned char)*a);
        int cb = ascii_lower((unsigned char)*b);
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

static int ascii_ci_prefix(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        if (!*s || ascii_lower((unsigned char)*s) != ascii_lower((unsigned char)*prefix))
            return 0;
    }
    return 1;
}

static uint32_t flash0_refuse(uint32_t rc, const char *reason, const char *detail) {
    fprintf(stderr, "flash0: refused (0x%08x): %s: %s\n", rc, reason, detail ? detail : "");
    return rc;
}

static Flash0PathKind flash0_parse(const char *path, const char **name_out) {
    const char *rest = path + (sizeof("flash0:") - 1u);
    if (strstr(rest, "..")) return FLASH0_PATH_TRAVERSAL;
    if (*rest != '/') return FLASH0_PATH_OUTSIDE;
    rest++;
    if (ascii_ci_equal(rest, "font") || ascii_ci_equal(rest, "font/"))
        return FLASH0_PATH_FONT_DIR;
    if (!ascii_ci_prefix(rest, "font/")) return FLASH0_PATH_OUTSIDE;
    const char *name = rest + (sizeof("font/") - 1u);
    if (!*name || strchr(name, '/') || strchr(name, '\\')) return FLASH0_PATH_NOT_A_NAME;
    *name_out = name;
    return FLASH0_PATH_FONT_FILE;
}

/* The slot a served name belongs to. A pending entry never matches. */
static int flash0_slot_for_name(const char *name, NkFontSlot *slot_out) {
    for (int i = 0; i < NK_FONT_SLOT_COUNT; i++) {
        const Flash0SlotEntry *entry = &s_flash0_slots[i];
        if (entry->measured && ascii_ci_equal(name, entry->served_name)) {
            *slot_out = (NkFontSlot)i;
            return 1;
        }
    }
    return 0;
}

/* Host-separator joins. The project root can be an extended Win32 path (\\?\...), where '/'
 * is not a separator, so the join uses nk_platform_path_separator() for every component. */
static int flash0_user_cache_path(char *out, size_t capacity, const char *user_data_dir,
                                  const char *file) {
    char sep = nk_platform_path_separator();
    int n = snprintf(out, capacity, "%s%c%s%c%s%c%s", user_data_dir, sep, NK_FONT_CACHE_PARENT,
                     sep, NK_FONT_CACHE_SUBDIR, sep, file);
    return n > 0 && (size_t)n < capacity;
}

static int flash0_project_path(char *out, size_t capacity, const char *project_dir,
                               const char *file) {
    char sep = nk_platform_path_separator();
    int n = snprintf(out, capacity, "%s%c%s", project_dir, sep, file);
    return n > 0 && (size_t)n < capacity;
}

/* Opens one candidate source and checks its size. A file that is empty or over the reader's
 * ceiling is refused by name and treated as absent, so the next source is tried. */
static int flash0_probe(const char *path, const char *label, const char *source,
                        FILE **fp_out, uint32_t *size_out) {
    FILE *fp = nk_fopen_utf8(path, "rb");
    if (!fp) return 0;
    long end = -1;
    if (fseek(fp, 0, SEEK_END) == 0) end = ftell(fp);
    if (end <= 0 || (unsigned long)end > NK_FONT_PGF_MAX_BYTES ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "flash0: %s font for slot '%s' refused: size is outside 1 byte to 16 MiB\n",
                source, label);
        fclose(fp);
        return 0;
    }
    *fp_out = fp;
    *size_out = (uint32_t)end;
    return 1;
}

/* Source order for one slot (FONT_PLAN 5.2): the user-imported cache, then the project's font,
 * then nothing. No cross-slot substitution. */
static Flash0Source flash0_resolve_path(const Flash0Sources *sources, NkFontSlot slot,
                                        char *path, size_t capacity,
                                        FILE **fp_out, uint32_t *size_out) {
    const char *file = s_flash0_slots[slot].served_name;
    const char *label = flash0_slot_label(slot);
    *fp_out = NULL;
    *size_out = 0;
    path[0] = '\0';
    if (!sources) return FLASH0_SOURCE_NONE;
    if (sources->user_data_dir[0] &&
        flash0_user_cache_path(path, capacity, sources->user_data_dir, file) &&
        flash0_probe(path, label, "user-imported", fp_out, size_out))
        return FLASH0_SOURCE_USER;
    if (sources->project_dir[0] &&
        flash0_project_path(path, capacity, sources->project_dir, file) &&
        flash0_probe(path, label, "project", fp_out, size_out))
        return FLASH0_SOURCE_PROJECT;
    path[0] = '\0';
    return FLASH0_SOURCE_NONE;
}

static Flash0Source flash0_resolve(const Flash0Sources *sources, NkFontSlot slot,
                                   FILE **fp_out, uint32_t *size_out) {
    char path[SR_FLASH0_PATH_MAX + 64];
    return flash0_resolve_path(sources, slot, path, sizeof(path), fp_out, size_out);
}

int sr_flash0_font_resolve_path(const Flash0Sources *sources, NkFontSlot slot, char *path_out,
                                size_t capacity, const char **source_out) {
    FILE *fp = NULL;
    uint32_t size = 0;
    if (source_out) *source_out = "none";
    if (!path_out || capacity == 0) return 0;
    path_out[0] = '\0';
    if ((unsigned)slot >= (unsigned)NK_FONT_SLOT_COUNT) return 0;
    Flash0Source source = flash0_resolve_path(sources, slot, path_out, capacity, &fp, &size);
    if (fp) fclose(fp);
    if (source == FLASH0_SOURCE_NONE) return 0;
    if (source_out) *source_out = source == FLASH0_SOURCE_USER ? "user-imported" : "project";
    return 1;
}

static uint32_t flash0_refuse_missing_slot(NkFontSlot slot, const char *guest_path) {
    char detail[SR_FLASH0_PATH_MAX + 128];
    snprintf(detail, sizeof(detail),
             "font slot '%s' has no source (no user-imported or project font); %s",
             flash0_slot_label(slot), guest_path);
    return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND, "missing slot", detail);
}

static uint32_t flash0_refuse_path(Flash0PathKind kind, const char *guest_path) {
    switch (kind) {
    case FLASH0_PATH_TRAVERSAL:
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND, "'..' is refused on flash0:", guest_path);
    case FLASH0_PATH_NOT_A_NAME:
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND, "not a served font file name", guest_path);
    default:
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND,
                             "flash0: serves only flash0:/font/", guest_path);
    }
}

int sr_flash0_font_is_device_path(const char *guest_path) {
    return guest_path && ascii_ci_prefix(guest_path, "flash0:");
}

uint32_t sr_flash0_font_refuse_write(const char *guest_path, const char *operation) {
    char detail[SR_FLASH0_PATH_MAX + 64];
    snprintf(detail, sizeof(detail), "%s on a read-only device: %s",
             operation ? operation : "write", guest_path ? guest_path : "");
    return flash0_refuse(SR_FLASH0_ERR_ACCESS, "write refused", detail);
}

uint32_t sr_flash0_font_open(const char *guest_path, uint32_t flags,
                             const Flash0Sources *sources,
                             FILE **host_out, uint32_t *size_out) {
    if (!guest_path || !host_out || !size_out) return SR_FLASH0_ERR_NOT_FOUND;
    *host_out = NULL;
    *size_out = 0;
    if (flags & FLASH0_WRITE_INTENT_FLAGS)
        return sr_flash0_font_refuse_write(guest_path, "open for write");
    const char *name = NULL;
    Flash0PathKind kind = flash0_parse(guest_path, &name);
    if (kind == FLASH0_PATH_FONT_DIR)
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND, "a directory is not a font file", guest_path);
    if (kind != FLASH0_PATH_FONT_FILE) return flash0_refuse_path(kind, guest_path);

    NkFontSlot slot = NK_FONT_SLOT_LATIN;
    if (!flash0_slot_for_name(name, &slot))
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND,
                             "no font slot serves this name (unknown, or pending measurement)",
                             guest_path);
    FILE *fp = NULL;
    uint32_t size = 0;
    Flash0Source source = flash0_resolve(sources, slot, &fp, &size);
    if (source == FLASH0_SOURCE_NONE) return flash0_refuse_missing_slot(slot, guest_path);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "flash0: slot=%s source=%s size=%u\n", flash0_slot_label(slot),
                source == FLASH0_SOURCE_USER ? "user" : "project", (unsigned)size);
    *host_out = fp;
    *size_out = size;
    return 0u;
}

uint32_t sr_flash0_font_stat(const char *guest_path, const Flash0Sources *sources,
                             uint32_t *size_out, int *is_dir_out) {
    if (!guest_path || !size_out || !is_dir_out) return SR_FLASH0_ERR_NOT_FOUND;
    *size_out = 0;
    *is_dir_out = 0;
    const char *name = NULL;
    Flash0PathKind kind = flash0_parse(guest_path, &name);
    if (kind == FLASH0_PATH_FONT_DIR) {
        *is_dir_out = 1;
        return 0u;
    }
    if (kind != FLASH0_PATH_FONT_FILE) return flash0_refuse_path(kind, guest_path);
    NkFontSlot slot = NK_FONT_SLOT_LATIN;
    if (!flash0_slot_for_name(name, &slot))
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND,
                             "no font slot serves this name (unknown, or pending measurement)",
                             guest_path);
    FILE *fp = NULL;
    uint32_t size = 0;
    if (flash0_resolve(sources, slot, &fp, &size) == FLASH0_SOURCE_NONE)
        return flash0_refuse_missing_slot(slot, guest_path);
    fclose(fp);
    *size_out = size;
    return 0u;
}

uint32_t sr_flash0_font_list_dir(const char *guest_path, const Flash0Sources *sources,
                                 SrVfsDirList *list) {
    const char *name = NULL;
    if (!guest_path || !list) return SR_FLASH0_ERR_NOT_FOUND;
    if (flash0_parse(guest_path, &name) != FLASH0_PATH_FONT_DIR)
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND, "only flash0:/font can be listed",
                             guest_path);
    list->exists = 1;
    for (int i = 0; i < NK_FONT_SLOT_COUNT; i++) {
        if (!s_flash0_slots[i].measured) continue;
        NkFontSlot slot = (NkFontSlot)i;
        FILE *fp = NULL;
        uint32_t size = 0;
        if (flash0_resolve(sources, slot, &fp, &size) == FLASH0_SOURCE_NONE) continue;
        fclose(fp);
        if (!sr_vfs_dirlist_merge_lba(list, s_flash0_slots[i].served_name, 0, size, 0u))
            return flash0_refuse(0x80010008u, "listing ran out of memory", guest_path);
    }
    sr_vfs_dirlist_sort(list);
    return 0u;
}

#ifdef SR_HLE_THREAD_SELFTEST
void sr_flash0_font_selftest_set_measured(NkFontSlot slot, int measured) {
    if ((unsigned)slot >= NK_FONT_SLOT_COUNT) return;
    s_flash0_slots[slot].measured = measured != 0;
}
#endif
