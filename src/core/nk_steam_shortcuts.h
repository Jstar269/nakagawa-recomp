/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Add to Steam (#2152): one non-Steam shortcut in a user's local Steam library.
 *
 * Steam keeps a user's non-Steam games in `<Steam>/userdata/<account id>/config/shortcuts.vdf`, a
 * binary KeyValues file (Valve's public binary VDF layout): a top-level map "shortcuts" whose
 * children "0", "1", ... are maps, one per game. Every entry is a type byte, a NUL-terminated
 * key and a value. The types this file uses are 0x00 (a nested map, closed by 0x08), 0x01 (a
 * NUL-terminated string), 0x02 (a little-endian int32) and 0x08 (the end of a map). The file
 * ends with 0x08 0x08: the end of "shortcuts" and the end of the root.
 *
 * Everything here works on the user's own machine and makes no network call. Nothing is
 * downloaded: the entry carries no icon or grid image beyond what the executable has.
 *
 * Rules (each has a test in tests/native/test_steam_shortcuts.c):
 *   - The file is only ever added to. The new entry is inserted before the end of "shortcuts"
 *     and every other byte, every entry Nakagawa did not write, stays byte for byte.
 *   - A file that does not parse as the layout above is NK_STEAM_ERR_CORRUPT and is left alone,
 *     with no backup and no temporary file. The parser reads untrusted bytes: every read is
 *     bounds-checked, nesting is capped and sizes are capped.
 *   - An entry whose executable is already listed is NK_STEAM_ERR_EXISTS: nothing is written.
 *   - Steam rewrites this file when it exits, so while Steam runs nothing is written
 *     (NK_STEAM_ERR_STEAM_RUNNING). Nakagawa never ends Steam.
 *   - Before a write the file is copied to `shortcuts.vdf.nakagawa-backup-<stamp>` and the copy
 *     is read back and compared. If that fails nothing is written (NK_STEAM_ERR_BACKUP). The
 *     new file goes to a temporary name, is flushed, and is renamed over the original; it is read
 *     back and compared with what was meant to be written.
 *   - Only Windows finds Steam itself (the registry). Elsewhere a caller names the Steam folder,
 *     or gets NK_STEAM_ERR_UNSUPPORTED; nothing pretends to succeed. */

#ifndef NK_STEAM_SHORTCUTS_H
#define NK_STEAM_SHORTCUTS_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_STEAM_OK = 0,
    NK_STEAM_ERR_ARGUMENT,       /* a missing, empty, too long or control-character field */
    NK_STEAM_ERR_CORRUPT,        /* shortcuts.vdf is not a file this code understands */
    NK_STEAM_ERR_EXISTS,         /* an entry for this executable is already there */
    NK_STEAM_ERR_TOO_LARGE,      /* the file is over NK_STEAM_VDF_MAX_BYTES */
    NK_STEAM_ERR_STEAM_RUNNING,  /* close Steam first */
    NK_STEAM_ERR_NOT_FOUND,      /* no Steam, no such account, or no account at all */
    NK_STEAM_ERR_BACKUP,         /* the backup could not be made and verified */
    NK_STEAM_ERR_WRITE,          /* the new file could not be written or did not verify */
    NK_STEAM_ERR_UNSUPPORTED     /* this host cannot find Steam by itself */
} NkSteamStatus;

/* A stable name for a status, for logs and cards (never NULL). */
const char *nk_steam_status_name(NkSteamStatus status);

#define NK_STEAM_VDF_MAX_BYTES (16u * 1024u * 1024u)
#define NK_STEAM_FIELD_MAX 4096u
#define NK_STEAM_ACCOUNT_CAP 16u

typedef struct {
    const char *app_name;        /* shown in the library; required */
    const char *exe;             /* the executable's full path, unquoted; required */
    const char *start_dir;       /* its start-in folder; required */
    const char *launch_options;  /* NULL or "" for none (the default) */
} NkSteamShortcut;

/* The app ID Steam derives for a non-Steam shortcut: the CRC-32 of the quoted executable path
 * followed by the name, with the top bit set. Steam shows it in the entry's "appid". */
uint32_t nk_steam_shortcut_app_id(const char *exe, const char *app_name);

/* The pure edit: `in` is the current shortcuts.vdf (NULL and 0 for no file), `*out` is a new
 * buffer holding it with the entry added, released with nk_steam_free. On any status other than
 * NK_STEAM_OK, *out is NULL. The result is parsed again before it is returned. */
NkSteamStatus nk_steam_vdf_add(const uint8_t *in, size_t in_len, const NkSteamShortcut *shortcut,
                               uint8_t **out, size_t *out_len);
void nk_steam_free(void *buffer);

/* Whether `in` lists an entry with this executable (compared without case, quotes ignored).
 * CORRUPT when it does not parse. */
NkSteamStatus nk_steam_vdf_has_exe(const uint8_t *in, size_t in_len, const char *exe, bool *found);

/* The file-level add: `vdf_path` is the full path of shortcuts.vdf (its folder is made when
 * missing and the file is created when absent). `steam_running` is the caller's answer to
 * "is Steam running"; true refuses. `stamp` is the backup suffix, 1 to 32 of [0-9A-Za-z-]
 * (the caller passes YYYYMMDD-HHMMSS). `backup_path` (optional) receives the backup's path, or
 * "" when there was no file to back up. */
NkSteamStatus nk_steam_add_to_file(const char *vdf_path, const NkSteamShortcut *shortcut,
                                   bool steam_running, const char *stamp,
                                   char *backup_path, size_t backup_cap);

/* ---- Finding Steam ---- */

typedef struct {
    char id[24];                       /* the account folder name: digits, the 32-bit account ID */
    char shortcuts_path[NK_MAX_PATH];  /* <Steam>/userdata/<id>/config/shortcuts.vdf */
} NkSteamAccount;

/* Steam's folder from this machine's own settings (Windows: HKCU\Software\Valve\Steam
 * SteamPath). NK_STEAM_ERR_UNSUPPORTED off Windows, NK_STEAM_ERR_NOT_FOUND when Steam is not
 * installed for this user or its folder is gone. */
NkSteamStatus nk_steam_find_root(char *root, size_t root_cap);

/* The accounts that have signed in on this PC: the all-digit folders of <root>/userdata other than
 * "0", in ascending order, at most `cap`. `*count` is set. NOT_FOUND when there are none. */
NkSteamStatus nk_steam_list_accounts(const char *root, NkSteamAccount *accounts, size_t cap,
                                     size_t *count);

/* Whether the Steam client runs now (Windows: the process ID Steam records and keeps current
 * while it runs). False when it cannot tell and when Steam is not running; off Windows always
 * false: that is not an answer, so nk_steam_add_for_account refuses there (UNSUPPORTED) unless a
 * caller supplies its own probe. */
bool nk_steam_client_running(void);

/* The whole step for one account: `account_id` must be one of the digit-only folder names of
 * nk_steam_list_accounts. `running` is nk_steam_client_running unless a caller (a test) names
 * another probe. */
typedef bool (*NkSteamRunningFn)(void);
NkSteamStatus nk_steam_add_for_account(const char *root, const char *account_id,
                                       const NkSteamShortcut *shortcut, NkSteamRunningFn running,
                                       const char *stamp, char *backup_path, size_t backup_cap);

#ifdef __cplusplus
}
#endif

#endif /* NK_STEAM_SHORTCUTS_H */
