/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_LIBRARY_H
#define NK_LIBRARY_H

#include "nk_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    NkGameEntry entries[NK_MAX_GAMES];
    int count;
    char library_path[NK_MAX_PATH];
} NkLibrary;

/* Initialize an empty library struct */
void nk_library_init(NkLibrary *lib);

/* Load library from disk. If file_path is NULL, loads from default app data directory */
NkResult nk_library_load(NkLibrary *lib, const char *file_path);

/* Save library to disk atomically. If file_path is NULL, uses lib->library_path */
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
