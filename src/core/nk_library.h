/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_LIBRARY_H
#define NK_LIBRARY_H

#include "nk_platform.h"
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

/* Save library to disk atomically, replacing whatever the file holds. A writer
   that shares the file with others changes it through nk_library_commit_entry or
   nk_library_commit_remove instead. If file_path is NULL, uses lib->library_path.
   Refuses with NK_ERROR_NEWER_SCHEMA, writing nothing, when the file there (or its
   .bak) was written by a newer player, and keeps an older file as
   library.json.v<N>.bak before replacing it. */
NkResult nk_library_save(const NkLibrary *lib, const char *file_path);

/* How long a library write waits for another writer before it gives up. */
#define NK_LIBRARY_LOCK_TIMEOUT_MS 30000u

/* Take the per-user library lock for the library file at library_path, waiting
   up to timeout_ms (0 tries once): locks/library.lock in the folder that holds
   library.json, the lock tools/nk_cli.py's bring-up holds while it reads, changes
   and saves the file. NK_ERROR_IO when another holder keeps it that long or the
   lock file cannot be opened. Release it with nk_platform_file_lock_release. */
NkResult nk_library_lock_acquire(NkFileLock *lock, const char *library_path,
                                 unsigned timeout_ms);

/* Write one entry to the library file under the library lock (#1768): the file
   is read again, entry replaces the record with its disc_id or is added, and the
   result is saved, so a title another writer added since lib was loaded is kept.
   lib then holds the saved records, in its own order with new ones after; its
   own records the file does not have (its samples, or one an earlier save could
   not write) stay in memory. A file a newer player wrote is refused
   (NK_ERROR_NEWER_SCHEMA); one that cannot be read at all, nor its .bak, is
   replaced from lib as nk_library_save would. If file_path is NULL, uses
   lib->library_path, then the default library. */
NkResult nk_library_commit_entry(NkLibrary *lib, const NkGameEntry *entry,
                                 const char *file_path);

/* Remove one entry from the library file the same way. An entry another writer
   already removed is not an error. */
NkResult nk_library_commit_remove(NkLibrary *lib, const char *disc_id,
                                  const char *file_path);

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
