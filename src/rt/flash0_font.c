/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#include "flash0_font.h"

#include <stdlib.h>
#include <string.h>

#include "../core/nk_platform.h"

/* PSP open flags (PSPSDK sceIo): WRONLY=0x0002 (RDWR also sets it), APPEND=0x0100,
 * CREAT=0x0200, TRUNC=0x0400. Any of them is a write intent. */
#define FLASH0_WRITE_INTENT_FLAGS 0x0702u

/* The reader's ceiling (src/rt/pgf_public.c). A larger file is refused, not truncated. */
#define FLASH0_FONT_MAX_BYTES (16u * 1024u * 1024u)

/* PENDING MEASUREMENT. Each entry is the name a slot is served under, measured from the
 * console (FONT_PLAN M4 for the lookup, M2 for the slot order). Every entry is pending:
 * served_name is NULL and measured is 0, so the slot is not listed and no open of it
 * succeeds. This table is the only place those names may be filled in, and only from the
 * measurement, never from a vendor file name. */
typedef struct {
    NkFontSlot slot;
    const char *served_name;  /* the name under flash0:/font/; NULL while pending */
    int measured;             /* 0 until the measurement lands */
} Flash0SlotEntry;

static Flash0SlotEntry s_flash0_slots[NK_FONT_SLOT_COUNT] = {
    { NK_FONT_SLOT_LATIN,    NULL, 0 },
    { NK_FONT_SLOT_JAPANESE, NULL, 0 },
    { NK_FONT_SLOT_KOREAN,   NULL, 0 },
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

/* The slot a served name belongs to, or NULL. A pending entry never matches. */
static Flash0SlotEntry *flash0_slot_for_name(const char *name) {
    for (size_t i = 0; i < NK_FONT_SLOT_COUNT; i++) {
        Flash0SlotEntry *entry = &s_flash0_slots[i];
        if (entry->measured && entry->served_name && ascii_ci_equal(name, entry->served_name))
            return entry;
    }
    return NULL;
}

/* Joins with the host separator. The project root can be an extended Win32 path (\\?\...),
 * where '/' is not a separator, so a '/' join would name a file that does not exist. */
static int flash0_build_path(char *out, size_t capacity, const char *dir,
                             const char *subdir, const char *stem) {
    char sep = nk_platform_path_separator();
    char rel[64];
    if (subdir) {
        if (strlen(subdir) >= sizeof(rel)) return 0;
        memcpy(rel, subdir, strlen(subdir) + 1u);
        for (char *p = rel; *p; p++)
            if (*p == '/') *p = sep;
    }
    int n = subdir ? snprintf(out, capacity, "%s%c%s%c%s.pgf", dir, sep, rel, sep, stem)
                   : snprintf(out, capacity, "%s%c%s.pgf", dir, sep, stem);
    return n > 0 && (size_t)n < capacity;
}

/* Opens one candidate source and checks its size. A file that is empty or over the
 * ceiling is refused by name and treated as absent, so the next source is tried. */
static int flash0_probe(const char *path, const char *stem, const char *source,
                        FILE **fp_out, uint32_t *size_out) {
    FILE *fp = nk_fopen_utf8(path, "rb");
    if (!fp) return 0;
    long end = -1;
    if (fseek(fp, 0, SEEK_END) == 0) end = ftell(fp);
    if (end <= 0 || (unsigned long)end > FLASH0_FONT_MAX_BYTES ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "flash0: %s font for slot '%s' refused: size is outside 1 byte to 16 MiB\n",
                source, stem);
        fclose(fp);
        return 0;
    }
    *fp_out = fp;
    *size_out = (uint32_t)end;
    return 1;
}

/* Source order for one slot (FONT_PLAN 5.2): the user-imported cache, then the project's
 * font, then nothing. No cross-slot substitution. */
static Flash0Source flash0_resolve(const Flash0Sources *sources, NkFontSlot slot,
                                   FILE **fp_out, uint32_t *size_out) {
    const char *stem = nk_font_slot_file_stem(slot);
    char path[SR_FLASH0_PATH_MAX + 64];
    *fp_out = NULL;
    *size_out = 0;
    if (!stem || !sources) return FLASH0_SOURCE_NONE;
    if (sources->user_data_dir[0] &&
        flash0_build_path(path, sizeof(path), sources->user_data_dir, NK_FONT_CACHE_SUBDIR, stem) &&
        flash0_probe(path, stem, "user-imported", fp_out, size_out))
        return FLASH0_SOURCE_USER;
    if (sources->project_dir[0] &&
        flash0_build_path(path, sizeof(path), sources->project_dir, NULL, stem) &&
        flash0_probe(path, stem, "project", fp_out, size_out))
        return FLASH0_SOURCE_PROJECT;
    return FLASH0_SOURCE_NONE;
}

static uint32_t flash0_refuse_missing_slot(NkFontSlot slot, const char *guest_path) {
    char detail[SR_FLASH0_PATH_MAX + 128];
    snprintf(detail, sizeof(detail),
             "font slot '%s' has no source (no user-imported or project font); %s",
             nk_font_slot_file_stem(slot), guest_path);
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

    Flash0SlotEntry *entry = flash0_slot_for_name(name);
    if (!entry)
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND,
                             "no font slot serves this name (unknown, or pending measurement)",
                             guest_path);
    FILE *fp = NULL;
    uint32_t size = 0;
    Flash0Source source = flash0_resolve(sources, entry->slot, &fp, &size);
    if (source == FLASH0_SOURCE_NONE) return flash0_refuse_missing_slot(entry->slot, guest_path);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "flash0: slot=%s source=%s size=%u\n",
                nk_font_slot_file_stem(entry->slot),
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
    Flash0SlotEntry *entry = flash0_slot_for_name(name);
    if (!entry)
        return flash0_refuse(SR_FLASH0_ERR_NOT_FOUND,
                             "no font slot serves this name (unknown, or pending measurement)",
                             guest_path);
    FILE *fp = NULL;
    uint32_t size = 0;
    if (flash0_resolve(sources, entry->slot, &fp, &size) == FLASH0_SOURCE_NONE)
        return flash0_refuse_missing_slot(entry->slot, guest_path);
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
    for (size_t i = 0; i < NK_FONT_SLOT_COUNT; i++) {
        Flash0SlotEntry *entry = &s_flash0_slots[i];
        if (!entry->measured || !entry->served_name) continue;
        FILE *fp = NULL;
        uint32_t size = 0;
        if (flash0_resolve(sources, entry->slot, &fp, &size) == FLASH0_SOURCE_NONE) continue;
        fclose(fp);
        if (!sr_vfs_dirlist_merge_lba(list, entry->served_name, 0, size, 0u))
            return flash0_refuse(0x80010008u, "listing ran out of memory", guest_path);
    }
    sr_vfs_dirlist_sort(list);
    return 0u;
}

#ifdef SR_HLE_THREAD_SELFTEST
void sr_flash0_font_selftest_bind(NkFontSlot slot, const char *served_name) {
    if ((unsigned)slot >= NK_FONT_SLOT_COUNT) return;
    s_flash0_slots[slot].served_name = served_name;
    s_flash0_slots[slot].measured = served_name != NULL;
}
#endif
