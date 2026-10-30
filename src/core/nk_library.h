/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_LIBRARY_H
#define NK_LIBRARY_H

#include "nk_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest library.json either reader accepts: room for NK_MAX_GAMES
   records as nk_library_save writes them. tools/nk_core/library.py holds the
   same limit (MAX_LIBRARY_JSON_BYTES). */
#define NK_LIBRARY_MAX_BYTES (16L * 1024L * 1024L)

/* The library.json schema this build reads and writes. Version 1 is the only
   one so far (v0.0.1 and later; source_image_path is an optional member older
   readers skip). A reader migrates every older version forward one step at a
   time; a newer file is refused with NK_ERROR_NEWER_SCHEMA and never rewritten
   (nk_user_data_schema.h). tools/nk_core/library.py holds the same number. */
#define NK_LIBRARY_SCHEMA_VERSION 1

typedef struct {
    NkGameEntry entries[NK_MAX_GAMES];
    int count;
    char library_path[NK_MAX_PATH];
    /* Set by nk_library_load when it returns NK_ERROR_NEWER_SCHEMA: the version
       the file on disk carries. 0 otherwise. */
    long newer_schema_version;
} NkLibrary;

/* Initialize an empty library struct */
void nk_library_init(NkLibrary *lib);

/* Load library from disk. If file_path is NULL, loads from default app data directory.
   A file (or, with no readable primary, a .bak) that a newer player wrote returns
   NK_ERROR_NEWER_SCHEMA with an empty library and newer_schema_version set; the
   older .bak is not used in its place, because saving it back would replace the
   newer file. */
NkResult nk_library_load(NkLibrary *lib, const char *file_path);

/* Save library to disk atomically. If file_path is NULL, uses lib->library_path.
   Refuses with NK_ERROR_NEWER_SCHEMA, writing nothing, when the file there (or its
   .bak) was written by a newer player, and keeps an older file as
   library.json.v<N>.bak before replacing it. */
NkResult nk_library_save(const NkLibrary *lib, const char *file_path);

/* Add or update a game entry in the library (keyed by disc_id) */
NkResult nk_library_add_or_update(NkLibrary *lib, const NkGameEntry *entry);

/* Remove a game entry from the library by disc_id */
NkResult nk_library_remove(NkLibrary *lib, const char *disc_id);

/* Lookup game entry by disc_id */
const NkGameEntry *nk_library_find_by_disc_id(const NkLibrary *lib, const char *disc_id);

/* Get game entry by index (0 .. count-1) */
const NkGameEntry *nk_library_get(const NkLibrary *lib, int index);

/* Count of entries in library */
int nk_library_count(const NkLibrary *lib);

#ifdef __cplusplus
}
#endif

#endif /* NK_LIBRARY_H */
