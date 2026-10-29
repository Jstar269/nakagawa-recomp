// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* *
 * Save operations write real files under memstick/PSP/SAVEDATA/<gameName><saveName>/ next to
 * the executable (override the root with SR_MEMSTICK), the same layout PPSSPP and a real PSP
 * use, so saves can be copied between this build and PPSSPP. Field offsets and result codes
 * follow PPSSPP's SceUtilitySavedataParam (Core/Dialog/SavedataParam.h); the earlier no-op
 * implementation was also missing the abortStatus word, putting msFree/idList/fileList four
 * bytes too low.
 *
 * Implemented modes: AUTOLOAD/LOAD/LISTLOAD read the data file back into dataBuf;
 * AUTOSAVE/SAVE/LISTSAVE write dataBuf plus PARAM.SFO and the icon/pic/snd blobs;
 * LIST enumerates this game's save directories; FILES lists a save's files; SIZES /
 * GETSIZE report the host Memory Stick's free space; the DELETE family removes a save directory.
 */

/* vfs_contained.h comes FIRST on purpose: on a strict-ISO POSIX build it must
 * select the feature profile before any system header is pulled in, otherwise
 * the descriptor-relative primitives it needs are hidden and the seam would
 * silently fall back to its fail-closed backend. */
#include "vfs_contained.h"
#include "recomp.h"
#include "vfs_path.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

/* No by-name deletion primitive is defined here any more. Destructive
 * savedata paths go through the contained-delete seam, which anchors on a
 * trusted root; sd_unlink/sd_rmdir existed only for the pathname design
 * that seam replaced, and leaving them defined would invite its return. */
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define sd_mkdir(path) _mkdir(path)
#define sd_stricmp(a, b) _stricmp((a), (b))
#else
#include <strings.h>
#include <unistd.h>
#define sd_mkdir(path) mkdir((path), 0777)
#define sd_stricmp(a, b) strcasecmp((a), (b))
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* A savedata filename is ONE path component chosen by the guest. The old check
 * only refused separators and a bare "..", so Win32 normalization could still
 * redirect the name: ADS streams ("x:stream"), reserved devices ("NUL",
 * "COM5"), wildcards and trailing dot/space aliasing all passed. Delegation to
 * sr_vfs_is_safe_component closes every one of those routes for both this
 * filter and the directory-entry filters below. */
static int path_sanitize(const char *name) {
    if (!name || !*name) return 0;
    return sr_vfs_is_safe_component(name, strlen(name));
}

static int path_join(char *out, size_t cap, const char *dir, const char *name) {
    if (!path_sanitize(name)) { errno = EINVAL; return 0; }
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1u + nl + 1u > cap) { errno = ENAMETOOLONG; return 0; }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl + 1u);
    return 1;
}

/* SceUtilitySavedataParam offsets */
enum {
    SDP_result    = 0x1c,
    SDP_mode      = 0x30,
    SDP_gameName  = 0x3c,   /* char[13] */
    SDP_saveName  = 0x4c,   /* char[20] */
    SDP_saveNameList = 0x60,
    SDP_fileName  = 0x64,   /* char[13] */
    SDP_dataBuf   = 0x74,
    SDP_dataBufSize = 0x78,
    SDP_dataSize  = 0x7c,
    SDP_sfoTitle  = 0x80,   /* char[0x80] */
    SDP_sfoSaveTitle = 0x100, /* char[0x80] */
    SDP_sfoDetail = 0x180,  /* char[0x400] */
    SDP_sfoParental = 0x580,
    SDP_icon0     = 0x584,  /* PspUtilitySavedataFileData: buf,bufSize,size,unk */
    SDP_icon1     = 0x594,
    SDP_pic1      = 0x5a4,
    SDP_snd0      = 0x5b4,
    SDP_msFree    = 0x5d0,
    SDP_msData    = 0x5d4,
    SDP_usedData  = 0x5d8,
    SDP_idList    = 0x5f4,
    SDP_fileList  = 0x5f8,
    SDP_sizeInfo  = 0x5fc,
};

/* SceUtilitySavedataType (PPSSPP SavedataParam.h). The MAKEDATA/READDATA/WRITEDATA/ERASE
 * family (13-21) is the no-dialog "secure" API many games use for the actual save IO after
 * presenting their own UI; PPSSPP routes them to the same Save/Load/Delete actions. */
enum { SD_AUTOLOAD=0, SD_AUTOSAVE=1, SD_LOAD=2, SD_SAVE=3, SD_LISTLOAD=4, SD_LISTSAVE=5,
       SD_LISTDELETE=6, SD_LISTALLDELETE=7, SD_SIZES=8, SD_AUTODELETE=9, SD_DELETE=10,
       SD_LIST=11, SD_FILES=12, SD_MAKEDATASECURE=13, SD_MAKEDATA=14, SD_READDATASECURE=15,
       SD_READDATA=16, SD_WRITEDATASECURE=17, SD_WRITEDATA=18, SD_ERASESECURE=19,
       SD_ERASE=20, SD_DELETEDATA=21, SD_GETSIZE=22 };

#define ERR_LOAD_NO_DATA    0x80110307u
#define ERR_LOAD_NOT_FOUND  0x80110309u
#define ERR_DELETE_NO_DATA  0x80110347u
#define ERR_RW_NO_DATA      0x80110327u
#define ERR_RW_NOT_FOUND    0x80110329u
#define ERR_RW_MS_FULL      0x80110323u
#define ERR_SIZES_NO_DATA   0x801103C7u   /* SIZES on a save that doesn't exist yet (PPSSPP) */
/* LOAD_DATA_BROKEN (PPSSPP Core/HLE/ErrorCodes.h).  PPSSPP's LoadNotCryptedSave
 * (Core/Dialog/SavedataParam.cpp, revision d40137a0) returns it in LOAD and
 * READDATA modes alike when the save data is larger than the guest buffer.
 * UNMEASURED: no PSP capture shows which code firmware returns for that case. */
#define ERR_LOAD_DATA_BROKEN_UNMEASURED 0x80110306u

/* SceUtilitySavedataParam: bind result (offset 0x34, after mode). PPSSPP sets 1021 on every
 * successful load -- "PSP always responds this and this unlocks some games". */
#define SDP_bind 0x34

/* The savedata utility counts the Memory Stick in 32 KiB sectors: on a PSP a
 * GETSIZE reports sectorSize 0x8000 (pspautotests tests/utility/savedata/
 * getsize.expected, revision 2ed7c9c5). */
#define CLUSTER 0x8000u

/* Host-volume failures. Neither code is measured on a PSP; the names and values
 * are PPSSPP's (Core/HLE/ErrorCodes.h). SIZES (8) uses its own family's access
 * error; GETSIZE (22) belongs to the MAKEDATA/READDATA/WRITEDATA family, which
 * has no access error, so it reports the stick as unavailable. */
#define ERR_SIZES_ACCESS_ERROR_UNMEASURED 0x801103C5u
#define ERR_RW_NO_MEMSTICK_UNMEASURED     0x80110321u

/* src/rt/hle.c: free bytes of the host-backed Memory Stick, clamped as the
 * capacity devctl clamps them (never above UINT32_MAX). 0 = query failed. */
int sr_hle_memstick_free_bytes(uint64_t *free_bytes);

/* PPSSPP's savedata model keeps file-list requests within 99 entries.  The
 * PSP GETSIZE structure has no separate array-capacity field, so this existing
 * savedata count contract is the semantic ceiling for both entry arrays here,
 * rather than an arbitrary time-based loop cap. */
#define SAVEDATA_MAX_FILE_ENTRIES 99u
#define SAVEDATA_SIZE_ENTRY_BYTES 24u
#define SAVEDATA_SIZE_INFO_BYTES 60u

static void rd_cstr(uint32_t addr, char *out, int max) {
    int i = 0;
    if (max <= 0) return;
    if (addr) for (; i < max - 1; i++) {
        uint8_t c = MEM_R8(addr + (uint32_t)i);
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static const char *ms_root(void) {
    return sr_ms0_root();
}

/* `save_dir` and the utility preparation paths are built from the same
 * operator-configured root.  This helper only recovers their already-known
 * root-relative spelling for descriptor traversal; it is not a string-prefix
 * containment check and no host operation is performed on its result. */
static int savedata_root_relative(const char *path, char *out, size_t cap) {
    const char *root = ms_root();
    size_t root_len = strlen(root);
    size_t path_len = path ? strlen(path) : 0;
    if (!path || !out || cap == 0 || root_len == 0 || path_len <= root_len ||
        strncmp(path, root, root_len) != 0 ||
        (path[root_len] != '/' && path[root_len] != '\\')) {
        return 0;
    }
    const char *rel = path + root_len;
    while (*rel == '/' || *rel == '\\') rel++;
    size_t rel_len = strlen(rel);
    if (rel_len == 0 || rel_len >= cap || !sr_cd_rel_is_acceptable(rel)) return 0;
    memcpy(out, rel, rel_len + 1u);
    return 1;
}

#ifdef _WIN32
/* Canonical identity of the memory-stick root for this process. The bare root
 * directory is created here on purpose (documented F114-4 side effect); every
 * deeper component is owned by sr_vfs_mkdirs_contained. */
static int ms_canonical_root(wchar_t *out, size_t cap) {
    return sr_vfs_canonical_root(ms_root(), out, cap);
}
#endif

static int mkdirs(const char *path) {
#ifdef _WIN32
    /* F114-2 ordering: resolve the canonical ROOT first, then create owned
     * components top-down with a containment verdict before anything deeper is
     * created. A junction pre-planted mid-path is rejected before it can
     * redirect a single CreateDirectory below it. The helper receives only the
     * owned tail below the root, so the operator-configured ancestors are
     * never created or rewritten here. */
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)))
        return 0;
    size_t root_len = strlen(ms_root());
    const char *tail = path + root_len;
    while (*tail == '/' || *tail == '\\') tail++;
    if (strncmp(path, ms_root(), root_len) != 0) return 0;
    return sr_vfs_mkdirs_contained(tail, canonical_root);
#else
#if defined(SR_CD_BACKEND_POSIX_AT)
    char rel[SR_CD_REL_MAX];
    if (!savedata_root_relative(path, rel, sizeof(rel))) return 0;
    /* The configured root is operator-owned, and creating that bare root is
     * the existing savedata preparation side effect.  All guest-named
     * components below it are still created only through descriptor-relative
     * mkdirat/openat traversal. */
    if (mkdir(ms_root(), 0777) != 0 && errno != EEXIST) return 0;
    sr_cd_root root;
    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;
    sr_cd_status st = sr_cd_mkdirs(&root, rel);
    sr_cd_root_close(&root);
    return st == SR_CD_OK;
#else
    /* An unsupported host must not fall back to pathname mkdir(). */
    (void)path;
    return 0;
#endif
#endif
}

/* Utility dialogs that do not perform a savedata operation still expect the memory-stick
 * hierarchy to exist. Keep that host policy here rather than duplicating Windows/POSIX path
 * handling in hle.c. mkdirs() treats an already-existing directory as success, so concurrent
 * utility workers can safely converge on the same roots. */
uint32_t sr_savedata_prepare_utility(unsigned kind) {
    char path[PATH_MAX];
    const char *leaf;
    switch (kind) {
        case 1: leaf = "PSP/SAVEDATA"; break; /* gamedata-install persistence */
        case 2: leaf = "PSP/GAME"; break;     /* game-sharing destination */
        default: return 0x80110004u;           /* SCE_ERROR_UTILITY_INVALID_PARAM */
    }
    if (snprintf(path, sizeof(path), "%s/%s", ms_root(), leaf) >= (int)sizeof(path))
        return 0x80110004u;
    return mkdirs(path) ? 0u : 0x80110001u;
}

/* memstick/PSP/SAVEDATA/<gameName><saveName> — resolved through the shared
 * sr_ms0_resolve so savedata and ordinary sceIo* agree on one host path. */
static void save_dir(char *out, int cap, const char *game, const char *save) {
    if (!path_sanitize(game) || (save && *save && !path_sanitize(save))) {
        snprintf(out, cap, "%s/PSP/SAVEDATA/INVALID", ms_root());
        return;
    }
    char leaf[64];
    int n = snprintf(leaf, sizeof(leaf), "%s%s", game, save ? save : "");
    if (n <= 0 || (size_t)n >= sizeof(leaf) || !sr_vfs_is_safe_component(leaf, (size_t)n)) {
        snprintf(out, cap, "%s/PSP/SAVEDATA/INVALID", ms_root());
        return;
    }
    char guest[96];
    if (snprintf(guest, sizeof(guest), "PSP/SAVEDATA/%s", leaf) >= (int)sizeof(guest)) {
        snprintf(out, cap, "%s/PSP/SAVEDATA/INVALID", ms_root());
        return;
    }
    if (!sr_ms0_resolve(ms_root(), guest, out, (size_t)cap, '/')) {
        snprintf(out, cap, "%s/PSP/SAVEDATA/INVALID", ms_root());
    }
}

/* Root-RELATIVE form of the same location: "PSP/SAVEDATA/<gameName><saveName>".
 * The contained-delete seam anchors on the trusted root itself and walks this
 * relative path component by component, so destructive savedata operations
 * never hand a host a re-resolvable absolute pathname.
 *
 * Unlike save_dir this validates the CONCATENATION as one component rather than
 * the two guest strings separately: "NU" and "L" are each a safe component but
 * "NUL" is a device alias, and it is the joined name that reaches the host. */
static int save_rel(char *out, size_t cap, const char *game, const char *save) {
    char leaf[64];
    if (!path_sanitize(game) || (save && *save && !path_sanitize(save))) return 0;
    int n = snprintf(leaf, sizeof(leaf), "%s%s", game, save ? save : "");
    if (n <= 0 || (size_t)n >= sizeof(leaf)) return 0;
    if (!sr_cd_component_is_generic(leaf, (size_t)n) ||
        !sr_vfs_is_safe_component(leaf, (size_t)n)) return 0;
    n = snprintf(out, cap, "PSP/SAVEDATA/%s", leaf);
    return n > 0 && (size_t)n < cap;
}

static int sdlog(void) { static int v = -1; if (v < 0) v = getenv("SR_DLGLOG") ? 1 : 0; return v; }

/* Issue #1387: one SAVE is one transaction over its member files: the data
 * file, PARAM.SFO and the icon, picture and sound blobs.
 *
 * Stage: host_write_file writes each member to a fresh temporary in the save
 * directory and flushes it to stable storage.  Nothing is published yet, so if
 * staging any member fails, every temporary is discarded and the SAVE fails
 * with no member touched: the previous save survives whole.
 *
 * Commit: only when every member is staged are the temporaries renamed over
 * their members, in reverse staging order.  Each rename replaces one member
 * atomically (POSIX renameat; on Win32 a rename through the temporary's own
 * handle that replaces an existing member, the same-directory operation
 * MoveFileExW performs for MOVEFILE_REPLACE_EXISTING, with POSIX semantics where
 * the host has them).  The data file is staged first and therefore published
 * last.  A rename that fails
 * (a host lock, a link or directory planted at a member name, an I/O error)
 * stops the commit, discards the remaining temporaries and fails the SAVE, so a
 * SAVE that reports failure never replaced the data file.
 *
 * Temporaries are named ".nakagawa-stage-<n>", one per staged member.  The name
 * is 17 bytes, longer than any savedata fileName (rd_cstr caps it at 15 bytes)
 * and unlike every system member name, so it never collides with a member.
 * Temporaries an interrupted run left behind are removed when the next SAVE of
 * that directory begins, and FILES and SIZES never report them. */
#define SAVEDATA_STAGE_PREFIX ".nakagawa-stage-"
#define SAVEDATA_STAGE_MAX 6u   /* data file, PARAM.SFO, ICON0, ICON1, PIC1, SND0 */

static int savedata_is_stage_name(const char *name) {
    return name && !strncmp(name, SAVEDATA_STAGE_PREFIX, sizeof(SAVEDATA_STAGE_PREFIX) - 1u);
}

static int stage_name(char *out, size_t cap, unsigned index) {
    int n = snprintf(out, cap, SAVEDATA_STAGE_PREFIX "%u", index);
    return n > 0 && (size_t)n < cap && path_sanitize(out);
}

/* Two member names that select the same host file.  Win32 compares names
 * case-insensitively, so there "param.sfo" and "PARAM.SFO" are one member. */
static int same_member(const char *a, const char *b) {
#ifdef _WIN32
    return sd_stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

typedef struct SavedataStaged {
    char name[16];              /* the member this temporary will replace */
    char tmp[24];               /* SAVEDATA_STAGE_PREFIX "<n>" */
    int staged;                 /* the temporary exists and is not published */
#ifdef _WIN32
    HANDLE h;                   /* the temporary's verified handle, kept to the rename */
#endif
} SavedataStaged;

typedef struct SavedataTxn {
    const char *dir;
    unsigned count;
    int open;
    SavedataStaged m[SAVEDATA_STAGE_MAX];
#ifdef _WIN32
    wchar_t canonical_root[MAX_PATH * 2];
#elif defined(SR_CD_BACKEND_POSIX_AT)
    sr_cd_root root;            /* one retained root for the whole transaction */
    char rel[SR_CD_REL_MAX];
#endif
} SavedataTxn;

#ifdef _WIN32
/* Rename the verified temporary THROUGH its own handle to `leaf` in the
 * directory that holds it.  Win32 resolves a bare leaf against the process's
 * current directory and refuses a RootDirectory handle (ERROR_INVALID_PARAMETER,
 * observed on Windows 11), so the destination is the handle's own verified
 * final directory path plus `leaf`.  The object is re-verified through the
 * handle after the rename; a directory swapped between the two steps fails the
 * save (the residual window is the one sceIoRename's MoveFileExW route has). */
static int host_replace_by_handle(HANDLE h, const char *leaf_utf8,
                                  const wchar_t *canonical_root) {
    wchar_t final_path[MAX_PATH * 2];
    const size_t cap = sizeof(final_path) / sizeof(final_path[0]);
    DWORD len = GetFinalPathNameByHandleW(h, final_path, (DWORD)cap,
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= cap) return 0;
    wchar_t *slash = wcsrchr(final_path, L'\\');
    if (!slash) return 0;
    size_t dir_chars = (size_t)(slash - final_path) + 1u;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, leaf_utf8, -1, NULL, 0);
    if (need <= 1 || dir_chars + (size_t)need > cap) return 0;
    size_t name_chars = dir_chars + (size_t)(need - 1);
    size_t total = offsetof(FILE_RENAME_INFO, FileName) + (name_chars + 1u) * sizeof(wchar_t);
    FILE_RENAME_INFO *info = (FILE_RENAME_INFO *)malloc(total);
    if (!info) return 0;
    memset(info, 0, total);
    info->RootDirectory = NULL;
    info->FileNameLength = (DWORD)(name_chars * sizeof(wchar_t));
    memcpy(info->FileName, final_path, dir_chars * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, leaf_utf8, -1,
                        info->FileName + dir_chars, need);
    BOOL ok = FALSE;
#if defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && defined(NTDDI_VERSION) && \
    NTDDI_VERSION >= 0x0A000002
    /* POSIX semantics replace a member that another handle holds open (a
     * guest sceIoOpen descriptor, which shares delete access), as renameat
     * does: that handle keeps reading the old file.  The classic rename
     * refuses such a member with ERROR_ACCESS_DENIED (observed on Windows 11),
     * which would fail every SAVE made while the game has its file open.
     * Windows before 10 1607 and volumes without POSIX semantics refuse the
     * class or the flag, and then get the classic rename. */
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    ok = SetFileInformationByHandle(h, FileRenameInfoEx, info, (DWORD)total);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED ||
            err == ERROR_CALL_NOT_IMPLEMENTED || err == ERROR_INVALID_FUNCTION) {
            info->Flags = 0;
            info->ReplaceIfExists = TRUE;
            ok = SetFileInformationByHandle(h, FileRenameInfo, info, (DWORD)total);
        }
    }
#else
    info->ReplaceIfExists = TRUE;
    ok = SetFileInformationByHandle(h, FileRenameInfo, info, (DWORD)total);
#endif
    free(info);
    return ok && sr_vfs_handle_is_contained(h, canonical_root);
}
#elif defined(SR_CD_BACKEND_POSIX_AT)
static int host_rel_join(char *out, size_t cap, const char *rel, const char *leaf) {
    int n = snprintf(out, cap, "%s/%s", rel, leaf);
    return n > 0 && (size_t)n < cap;
}
#endif

/* Stage one member: write its `n` bytes to the member's fresh temporary and
 * flush them to stable storage.  The published member is not touched. */
static int host_write_file(SavedataTxn *t, SavedataStaged *s, const uint8_t *data, uint32_t n) {
    if (n > 0 && !data) { errno = EINVAL; return 0; }
#ifdef _WIN32
    char path[PATH_MAX];
    if (!path_join(path, sizeof(path), t->dir, s->tmp)) return 0;
    /* OPEN -> HANDLE -> FINAL PATH VERIFY -> OPERATION.  CREATE_NEW makes a
     * new object, so nothing planted at the temporary's name (a link, a hard
     * link, a stale file) is ever opened or truncated, and the bytes land
     * through the very handle whose final path was verified.  DELETE access
     * lets the same handle rename or discard the temporary later. */
    HANDLE h;
    if (!sr_vfs_open_contained_utf8(path, GENERIC_READ | GENERIC_WRITE | DELETE,
                                    FILE_ATTRIBUTE_NORMAL, CREATE_NEW, t->canonical_root, &h))
        return 0;
    DWORD written = 0;
    int ok = !n || (WriteFile(h, data, (DWORD)n, &written, NULL) && written == (DWORD)n);
    if (ok) ok = FlushFileBuffers(h) != 0;
    if (!ok) {
        (void)sr_vfs_dispose_by_handle(h);
        CloseHandle(h);
        return 0;
    }
    s->h = h;
    s->staged = 1;
    return 1;
#else
#if defined(SR_CD_BACKEND_POSIX_AT)
    /* The seam opens the temporary with O_NOFOLLOW and refuses anything but a
     * regular file; txn_begin already unlinked any stale object of its name. */
    FILE *f = NULL;
    sr_cd_status st = sr_cd_open_file(&t->root, t->rel, s->tmp, SR_CD_FILE_WRITE_TRUNCATE, &f);
    int ok = st == SR_CD_OK && f;
    if (ok) {
        ok = !n || fwrite(data, 1, n, f) == n;
        if (ok && fflush(f) != 0) ok = 0;
        if (ok && fsync(fileno(f)) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
    }
    if (!ok) {
        (void)sr_cd_delete_leaf(&t->root, t->rel, s->tmp);
        return 0;
    }
    s->staged = 1;
    return 1;
#else
    (void)t; (void)s; (void)data; (void)n;
    return 0;
#endif
#endif
}

static int host_read_file(const char *dir, const char *name, uint8_t *data, uint32_t cap,
                          uint32_t *size) {
    if (!path_sanitize(name)) { errno = EINVAL; return 0; }
#ifdef _WIN32
    char path[PATH_MAX];
    if (!path_join(path, sizeof(path), dir, name)) return 0;
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)))
        return 0;
    HANDLE h;
    if (!sr_vfs_open_contained_utf8(path, GENERIC_READ, FILE_ATTRIBUTE_NORMAL,
                                    OPEN_EXISTING, canonical_root, &h))
        return 0;
    DWORD bytes_read = 0;
    BOOL ok = TRUE;
    if (data && cap > 0) {
        ok = ReadFile(h, data, (DWORD)cap, &bytes_read, NULL);
    } else {
        /* Size probe (data == NULL): report the verified object's real size. */
        LARGE_INTEGER sz;
        if (GetFileSizeEx(h, &sz)) {
            if (sz.QuadPart >= 0 && sz.QuadPart <= UINT32_MAX) bytes_read = (DWORD)sz.QuadPart;
            else ok = FALSE;
        } else {
            ok = FALSE;
        }
    }
    CloseHandle(h);
    if (size) *size = (uint32_t)bytes_read;
    return ok ? 1 : 0;
#else
#if defined(SR_CD_BACKEND_POSIX_AT)
    char rel[SR_CD_REL_MAX];
    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;
    sr_cd_root root;
    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;
    FILE *f = NULL;
    sr_cd_status st = sr_cd_open_file(&root, rel, name, SR_CD_FILE_READ, &f);
    sr_cd_root_close(&root);
    if (st != SR_CD_OK || !f) return 0;
    uint32_t n = 0;
    int ok = 1;
    if (data && cap) {
        n = (uint32_t)fread(data, 1, cap, f);
        ok = !ferror(f);
    } else {
        /* Size probe: callers pass a NULL buffer to learn the verified file's
         * real size before a bounded second read. */
        long sz = -1;
        if (fseek(f, 0, SEEK_END) == 0) {
            sz = ftell(f);
            if (fseek(f, 0, SEEK_SET) != 0) sz = -1;
        }
        ok = sz >= 0;
        if (ok) n = (uint32_t)sz;
    }
    if (fclose(f) != 0) ok = 0;
    if (size) *size = n;
    return ok;
#else
    return 0;
#endif
#endif
}

/* ---- the SAVE transaction (issue #1387; see host_write_file) ---------------------------- */

/* Remove one staged temporary that will not be published. */
static void host_discard(SavedataTxn *t, SavedataStaged *s) {
    if (!s->staged) return;
#ifdef _WIN32
    (void)t;
    (void)sr_vfs_dispose_by_handle(s->h);
    CloseHandle(s->h);
    s->h = NULL;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    (void)sr_cd_delete_leaf(&t->root, t->rel, s->tmp);
#else
    (void)t;
#endif
    s->staged = 0;
}

/* Discard every unpublished temporary and end the transaction. */
static void txn_abort(SavedataTxn *t) {
    for (unsigned i = 0; i < t->count; i++) host_discard(t, &t->m[i]);
#if !defined(_WIN32) && defined(SR_CD_BACKEND_POSIX_AT)
    if (t->open) sr_cd_root_close(&t->root);
#endif
    t->open = 0;
    t->count = 0;
}

static int txn_begin(SavedataTxn *t, const char *dir) {
    memset(t, 0, sizeof(*t));
    t->dir = dir;
#if defined(_WIN32) || defined(SR_CD_BACKEND_POSIX_AT)
#ifdef _WIN32
    if (!ms_canonical_root(t->canonical_root, sizeof(t->canonical_root) / sizeof(wchar_t)))
        return 0;
#else
    if (!savedata_root_relative(dir, t->rel, sizeof(t->rel))) return 0;
    if (sr_cd_root_open(ms_root(), &t->root) != SR_CD_OK) return 0;
#endif
    t->open = 1;
    /* Remove what an interrupted SAVE left behind.  Each removal is bound to
     * this directory and never follows a link; a directory planted at a
     * temporary's name is refused here and then fails that member's stage. */
    for (unsigned i = 0; i < SAVEDATA_STAGE_MAX; i++) {
        char tmp[24];
        if (!stage_name(tmp, sizeof(tmp), i)) { txn_abort(t); return 0; }
#ifdef _WIN32
        char path[PATH_MAX];
        int was_dir = 0;
        if (!path_join(path, sizeof(path), dir, tmp)) { txn_abort(t); return 0; }
        (void)sr_vfs_delete_contained_leaf(path, t->canonical_root, &was_dir);
#else
        (void)sr_cd_delete_leaf(&t->root, t->rel, tmp);
#endif
    }
    return 1;
#else
    return 0;   /* an unsupported host has no contained write route */
#endif
}

/* Stage `name`.  A member written twice in one SAVE keeps its later bytes, as
 * the sequential writes this transaction replaced did. */
static int txn_stage(SavedataTxn *t, const char *name, const uint8_t *data, uint32_t n) {
    size_t len = name ? strlen(name) : 0;
    if (!t->open || !path_sanitize(name) || len >= sizeof(t->m[0].name)) {
        errno = EINVAL;
        return 0;
    }
    unsigned slot = t->count;
    for (unsigned i = 0; i < t->count; i++) {
        if (same_member(t->m[i].name, name)) { slot = i; break; }
    }
    if (slot >= SAVEDATA_STAGE_MAX) { errno = ENOSPC; return 0; }
    SavedataStaged *s = &t->m[slot];
    host_discard(t, s);
    if (slot == t->count) {
        if (!stage_name(s->tmp, sizeof(s->tmp), slot)) return 0;
        t->count++;
    }
    memcpy(s->name, name, len + 1u);
    return host_write_file(t, s, data, n);
}

/* Publish every staged member, the data file last, and end the transaction.
 * Returns 0 when any rename failed; members not yet renamed keep their
 * previous contents and their temporaries are discarded. */
static int txn_commit(SavedataTxn *t) {
    int ok = t->open;
    for (unsigned i = t->count; ok && i-- > 0;) {
        SavedataStaged *s = &t->m[i];
        if (!s->staged) { ok = 0; break; }
#ifdef _WIN32
        ok = host_replace_by_handle(s->h, s->name, t->canonical_root);
        if (ok) {
            CloseHandle(s->h);
            s->h = NULL;
            s->staged = 0;
        }
#elif defined(SR_CD_BACKEND_POSIX_AT)
        char tmp_rel[SR_CD_REL_MAX], dst_rel[SR_CD_REL_MAX];
        /* sr_cd_rename_file refuses a destination that is a link, directory or
         * special file, and renameat never follows either final component. */
        ok = host_rel_join(tmp_rel, sizeof(tmp_rel), t->rel, s->tmp) &&
             host_rel_join(dst_rel, sizeof(dst_rel), t->rel, s->name) &&
             sr_cd_rename_file(&t->root, tmp_rel, dst_rel) == SR_CD_OK;
        if (ok) s->staged = 0;
#else
        ok = 0;
#endif
    }
    txn_abort(t);
    return ok;
}

/* Storage boundary: PSP serialization/crypto can be inserted by replacing these operations;
 * dialog and guest-memory code never depends on the host filesystem representation. */
typedef struct SavedataStorageOps {
    int (*prepare)(const char *dir);
    int (*begin)(SavedataTxn *txn, const char *dir);
    int (*stage)(SavedataTxn *txn, const char *name, const uint8_t *data, uint32_t size);
    int (*commit)(SavedataTxn *txn);
    void (*abort)(SavedataTxn *txn);
    int (*read)(const char *dir, const char *name, uint8_t *data, uint32_t cap, uint32_t *size);
} SavedataStorageOps;

static const SavedataStorageOps s_storage = {
    mkdirs, txn_begin, txn_stage, txn_commit, txn_abort, host_read_file
};

/* ---- minimal PARAM.SFO writer (PSF v1.1) -------------------------------------------------- */
typedef struct { const char *key; uint16_t fmt; uint32_t len, maxlen; const void *val; } SfoEnt;

static SfoEnt sfo_str_entry(const char *key, const char *val, uint32_t maxlen) {
    if (!val) val = "";
    size_t slen = strlen(val);
    if (maxlen > 0 && slen >= (size_t)maxlen) {
        slen = (size_t)maxlen - 1u;
    }
    uint32_t len = maxlen > 0 ? (uint32_t)slen + 1u : 0u;
    return (SfoEnt){key, 0x0204, len, maxlen, val};
}

static int sfo_write(SavedataTxn *txn, const char *title, const char *saveTitle,
                     const char *detail, const char *saveDir, uint32_t parental,
                     const char *fileName) {
    static uint8_t params[128], filelist[3168];   /* zeroed secure blocks */
    /* FILE_LIST entry 0: {char name[13]; u8 hash[16]; u8 pad[3]} -- records the data file
     * (hash left zero; we don't encrypt). */
    memset(filelist, 0, sizeof(filelist));
    if (fileName && fileName[0]) {
        size_t fl = strlen(fileName); if (fl > 12) fl = 12;
        memcpy(filelist, fileName, fl);
    }
    char cat[4] = "MS";
    uint32_t pl = parental;
    SfoEnt e[8];
    int n = 0;
    /* keys must be sorted alphabetically */
    e[n++] = sfo_str_entry("CATEGORY",           cat, 4);
    e[n++] = (SfoEnt){"PARENTAL_LEVEL",          0x0404, 4, 4, &pl};
    e[n++] = sfo_str_entry("SAVEDATA_DETAIL",    detail, 1024);
    e[n++] = sfo_str_entry("SAVEDATA_DIRECTORY", saveDir, 64);
    e[n++] = (SfoEnt){"SAVEDATA_FILE_LIST",      0x0004, sizeof(filelist), sizeof(filelist), filelist};
    e[n++] = (SfoEnt){"SAVEDATA_PARAMS",         0x0004, sizeof(params), sizeof(params), params};
    e[n++] = sfo_str_entry("SAVEDATA_TITLE",     saveTitle, 128);
    e[n++] = sfo_str_entry("TITLE",              title, 128);

    uint32_t keyOff[8], keySize = 0, dataOff[8], dataSize = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].len > e[i].maxlen) return 0;
        keyOff[i] = keySize; keySize += (uint32_t)strlen(e[i].key) + 1;
        dataOff[i] = dataSize; dataSize += (e[i].maxlen + 3) & ~3u;
    }
    uint32_t keyStart = 20 + 16u * (uint32_t)n;
    uint32_t dataStart = (keyStart + keySize + 3) & ~3u;
    uint32_t totalAlloc = dataStart + dataSize;

    uint8_t *buf = (uint8_t *)calloc(1, totalAlloc);
    if (!buf) return 0;
    memcpy(buf, "\0PSF", 4);
    *(uint32_t *)(buf + 4) = 0x0101;
    *(uint32_t *)(buf + 8) = keyStart;
    *(uint32_t *)(buf + 12) = dataStart;
    *(uint32_t *)(buf + 16) = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *ix = buf + 20 + 16 * i;
        *(uint16_t *)(ix + 0) = (uint16_t)keyOff[i];
        *(uint16_t *)(ix + 2) = e[i].fmt;
        *(uint32_t *)(ix + 4) = e[i].len;
        *(uint32_t *)(ix + 8) = e[i].maxlen;
        *(uint32_t *)(ix + 12) = dataOff[i];
        strcpy((char *)(buf + keyStart + keyOff[i]), e[i].key);
        if (e[i].fmt == 0x0204) {
            uint32_t payload = e[i].len > 0 ? e[i].len - 1u : 0u;
            if (payload > 0) {
                memcpy(buf + dataStart + dataOff[i], e[i].val, payload);
            }
            /* The preflight above proved len <= maxlen, so payload < maxlen for
             * every slot that has capacity at all.  A zero-capacity slot has no
             * terminator byte to write: writing one unconditionally would put a
             * byte into the NEXT entry's slot, or past the allocation entirely
             * when the zero-capacity entry is last.  No caller passes maxlen 0
             * today; the guard is what keeps that true of the helper rather than
             * only of its current call sites. */
            if (e[i].maxlen > 0) {
                buf[dataStart + dataOff[i] + payload] = '\0';
            }
        } else {
            memcpy(buf + dataStart + dataOff[i], e[i].val, e[i].len);
        }
    }
    int ok = s_storage.stage(txn, "PARAM.SFO", buf, totalAlloc);
    free(buf);
    return ok;
}

/* ---- ScePspDateTime (16 bytes) from a standard time_t ------------------------------------- */
static void put_psp_time(uint32_t addr, time_t when) {
    struct tm value;
    memset(&value, 0, sizeof(value));
#ifdef _WIN32
    if (when != (time_t)-1) localtime_s(&value, &when);
#else
    if (when != (time_t)-1) localtime_r(&when, &value);
#endif
    MEM_W16(addr + 0, (uint16_t)(value.tm_year + 1900));
    MEM_W16(addr + 2, (uint16_t)(value.tm_mon + 1)); MEM_W16(addr + 4, (uint16_t)value.tm_mday);
    MEM_W16(addr + 6, (uint16_t)value.tm_hour); MEM_W16(addr + 8, (uint16_t)value.tm_min);
    MEM_W16(addr + 10, (uint16_t)value.tm_sec); MEM_W32(addr + 12, 0);
}

static void wr_fixed(uint32_t addr, const char *s, int n) {
    size_t slen = s ? strlen(s) : 0;
    for (int i = 0; i < n; i++) MEM_W8(addr + (uint32_t)i, (size_t)i < slen ? (uint8_t)s[i] : 0);
}

/* ---- minimal PARAM.SFO reader --------------------------------------------------------------
 * Load must hand the SFO strings BACK to the game (PPSSPP SavedataParam::LoadSFO): games keep
 * their profile/pilot name in SAVEDATA_TITLE and read sfoParam after a load -- without this
 * the name they saved comes back empty. */
static void wr_guest_bytes(uint32_t addr, const uint8_t *v, uint32_t len, uint32_t cap) {
    if (len > cap) len = cap;
    for (uint32_t i = 0; i < len; i++) MEM_W8(addr + i, v[i]);
    for (uint32_t i = len; i < cap; i++) MEM_W8(addr + i, 0);
}

static void load_sfo_param(uint32_t param, const char *dir) {
    /* Routed through the storage boundary so the PARAM.SFO read is subject to
     * exactly the same containment verification as guest data files. */
    uint32_t n = 0;
    if (!s_storage.read(dir, "PARAM.SFO", NULL, 0, &n)) return;
    if (n <= 20 || n > (1 << 20)) return;

    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) return;
    uint32_t actual_read = 0;
    if (!s_storage.read(dir, "PARAM.SFO", b, n, &actual_read) || actual_read != n) {
        free(b);
        return;
    }
    if (memcmp(b, "\0PSF", 4) != 0) { free(b); return; }
    uint32_t keyStart, dataStart, cnt;
    memcpy(&keyStart, b + 8, 4); memcpy(&dataStart, b + 12, 4); memcpy(&cnt, b + 16, 4);
    /* The index table is a file envelope of its own.  Reject it before the
     * loop so a forged count cannot make the parser walk a truncated table or
     * leave a partially published set of guest fields. */
    if (keyStart > (uint32_t)n || dataStart > (uint32_t)n ||
        cnt > ((uint32_t)n - 20u) / 16u) {
        free(b);
        return;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *ix = b + 20 + 16 * i;
        uint16_t keyOff, fmt; uint32_t len, dataOff;
        memcpy(&keyOff, ix + 0, 2); memcpy(&fmt, ix + 2, 2);
        memcpy(&len, ix + 4, 4); memcpy(&dataOff, ix + 12, 4);
        uint64_t keyPos = (uint64_t)keyStart + keyOff;
        uint64_t dataPos = (uint64_t)dataStart + dataOff;
        if (keyPos >= (uint64_t)n || dataPos > (uint64_t)n ||
            (uint64_t)len > (uint64_t)n - dataPos) continue;
        const uint8_t *keyBytes = b + (size_t)keyPos;
        size_t keyRemain = (size_t)((uint64_t)n - keyPos);
        size_t keyLen = 0;
        while (keyLen < keyRemain && keyBytes[keyLen] != '\0') keyLen++;
        if (keyLen == keyRemain) continue;  /* unterminated key */
        const char *key = (const char *)keyBytes;
        const uint8_t *val = b + (size_t)dataPos;
        if (fmt == 0x0204 && !strcmp(key, "TITLE"))
            wr_guest_bytes(param + SDP_sfoTitle, val, len, 128);
        else if (fmt == 0x0204 && !strcmp(key, "SAVEDATA_TITLE"))
            wr_guest_bytes(param + SDP_sfoSaveTitle, val, len, 128);
        else if (fmt == 0x0204 && !strcmp(key, "SAVEDATA_DETAIL"))
            wr_guest_bytes(param + SDP_sfoDetail, val, len, 1024);
        else if (fmt == 0x0404 && len >= 4 && !strcmp(key, "PARENTAL_LEVEL")) {
            uint32_t pl; memcpy(&pl, val, 4);
            MEM_W32(param + SDP_sfoParental, pl);
        }
    }
    free(b);
}

/* ---- modes -------------------------------------------------------------------------------- */

static int validate_filedata_readable(uint32_t fd) {
    if (!fd) return 1;
    uint32_t buf = MEM_R32(fd + 0), sz = MEM_R32(fd + 8);
    return sr_guest_span_readable_current(buf, sz);
}

static int validate_filedata_writable(uint32_t fd) {
    if (!fd) return 1;
    uint32_t buf = MEM_R32(fd + 0), cap = MEM_R32(fd + 4);
    return sr_guest_span_writable(buf, cap);
}

/* Stage one PspUtilitySavedataFileData blob.  An absent blob is no member and
 * succeeds; a present one that cannot be staged fails the SAVE. */
static int write_filedata(SavedataTxn *txn, const char *name, uint32_t fd) {
    uint32_t buf = MEM_R32(fd + 0), sz = MEM_R32(fd + 8);
    if (!buf || !sz) return 1;
    if (!sr_guest_span_readable_current(buf, sz)) return 0;
    return s_storage.stage(txn, name, (const uint8_t *)SR_HOST(buf), sz);
}

static uint32_t do_save(uint32_t param, const char *game, const char *save) {
    char dir[PATH_MAX], fileName[16], title[128], saveTitle[128], detail[1024], saveDir[64];
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    uint32_t dataBuf = MEM_R32(param + SDP_dataBuf);
    uint32_t dataSize = MEM_R32(param + SDP_dataSize);

    /* Atomic preflight: validate guest buffers and the guest-chosen filename
     * before any host directory creation or file write. */
    if (fileName[0] && !path_sanitize(fileName)) return 0x80110381u;
    if (fileName[0] && dataSize) {
        if (!dataBuf || dataSize >= 0x04000000u ||
            !sr_guest_span_readable_current(dataBuf, dataSize))
            return 0x80110381u;
    }
    if (!validate_filedata_readable(param + SDP_icon0) ||
        !validate_filedata_readable(param + SDP_icon1) ||
        !validate_filedata_readable(param + SDP_pic1) ||
        !validate_filedata_readable(param + SDP_snd0)) {
        return 0x80110381u;
    }

    save_dir(dir, sizeof(dir), game, save);
    if (!s_storage.prepare(dir)) return 0x80110381u;
    /* Issue #1387: every member is staged before any is published, so a
     * failure anywhere leaves the previous save as it was (host_write_file). */
    SavedataTxn txn;
    if (!s_storage.begin(&txn, dir)) return 0x80110381u;
    int ok = 1;
    if (fileName[0] && dataBuf && dataSize && dataSize < 0x04000000)
        ok = s_storage.stage(&txn, fileName, (const uint8_t *)SR_HOST(dataBuf), dataSize);
    rd_cstr(param + SDP_sfoTitle, title, sizeof(title));
    rd_cstr(param + SDP_sfoSaveTitle, saveTitle, sizeof(saveTitle));
    rd_cstr(param + SDP_sfoDetail, detail, sizeof(detail));
    snprintf(saveDir, sizeof(saveDir), "%s%s", game, save);
    ok = ok && sfo_write(&txn, title, saveTitle, detail, saveDir,
                         MEM_R8(param + SDP_sfoParental), fileName);
    ok = ok && write_filedata(&txn, "ICON0.PNG", param + SDP_icon0);
    ok = ok && write_filedata(&txn, "ICON1.PMF", param + SDP_icon1);
    ok = ok && write_filedata(&txn, "PIC1.PNG", param + SDP_pic1);
    ok = ok && write_filedata(&txn, "SND0.AT3", param + SDP_snd0);
    if (!ok) {
        s_storage.abort(&txn);
        if (sdlog()) fprintf(stderr, "savedata: SAVE %s -> staging failed, previous save kept\n", dir);
        return 0x80110381u;   /* SAVE_NO_MS: couldn't write */
    }
    if (!s_storage.commit(&txn)) {
        if (sdlog()) fprintf(stderr, "savedata: SAVE %s -> publishing failed\n", dir);
        return 0x80110381u;
    }
    if (sdlog()) fprintf(stderr, "savedata: SAVE %s\\%s (%" PRIu32 " bytes)\n", dir, fileName, dataSize);
    return 0;
}

static int dir_exists(const char *dir) {
#ifdef _WIN32
    /* A directory that resolves outside the memory-stick root (a junction, a
     * device alias) does not exist as far as savedata is concerned. */
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)))
        return 0;
    if (!sr_vfs_dir_is_contained(dir, canonical_root)) return 0;
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
#else
#if defined(SR_CD_BACKEND_POSIX_AT)
    char rel[SR_CD_REL_MAX];
    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;
    sr_cd_root root;
    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;
    sr_cd_status st = sr_cd_dir_is_contained(&root, rel);
    sr_cd_root_close(&root);
    return st == SR_CD_OK;
#else
    return 0;
#endif
#endif
}

/* Read a host file into a guest PspUtilitySavedataFileData block {buf, bufSize, size, unk}.
 * PPSSPP loads ICON0/ICON1/PIC1/SND0 back on every Load; some games require it. */
static void load_filedata(const char *dir, const char *name, uint32_t fd) {
    uint32_t buf = MEM_R32(fd + 0), cap = MEM_R32(fd + 4);
    if (!buf || !cap) return;
    if (!sr_guest_span_writable(buf, cap)) return;
    uint32_t rd = 0;
    sr_guest_vram_write_prepare_bytes(buf, cap);
    int loaded = s_storage.read(dir, name, (uint8_t *)SR_HOST(buf), cap, &rd);
    sr_guest_vram_write_commit_bytes(buf, rd);
    if (loaded) {
        MEM_W32(fd + 8, rd);
    }
}

static uint32_t do_load(uint32_t param, const char *game, const char *save) {
    char dir[PATH_MAX], fileName[16];
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    uint32_t dataBuf = MEM_R32(param + SDP_dataBuf);
    uint32_t cap = MEM_R32(param + SDP_dataBufSize);

    /* Atomic preflight: validate output buffers and the guest-chosen filename
     * before mutating guest SDP_dataSize or guest registers. */
    if (fileName[0] && !path_sanitize(fileName)) return ERR_LOAD_NOT_FOUND;
    if (fileName[0] && (dataBuf || cap)) {
        if (!dataBuf || cap == 0 || cap >= 0x04000000u || !sr_guest_span_writable(dataBuf, cap))
            return ERR_LOAD_NO_DATA;
    }
    if (!validate_filedata_writable(param + SDP_icon0) ||
        !validate_filedata_writable(param + SDP_icon1) ||
        !validate_filedata_writable(param + SDP_pic1) ||
        !validate_filedata_writable(param + SDP_snd0)) {
        return ERR_LOAD_NO_DATA;
    }

    save_dir(dir, sizeof(dir), game, save);
    if (!dir_exists(dir)) {
        if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> no data\n", dir);
        return ERR_LOAD_NO_DATA;
    }
    MEM_W32(param + SDP_dataSize, 0);
    /* Blank fileName means success without reading data (PPSSPP LoadSaveData); the SFO and
     * icon blobs are still handed back. */
    if (fileName[0]) {
        uint32_t n = 0;
        if (!s_storage.read(dir, fileName, NULL, 0, &n)) {
            if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> file not found\n", dir);
            return ERR_LOAD_NOT_FOUND;
        }
        /* Issue #1387: data that does not fit the guest buffer is refused, not
         * cut to the buffer size and reported as a success.  The code follows
         * PPSSPP; the one a PSP returns here is UNMEASURED. */
        if (dataBuf && (uint64_t)n > (uint64_t)cap) {
            if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> %u bytes exceed buffer %" PRIu32 "\n",
                                 dir, (unsigned)n, cap);
            return ERR_LOAD_DATA_BROKEN_UNMEASURED;
        }
        size_t to_read = (size_t)n;
        uint32_t rd = 0;
        if (dataBuf && to_read > 0) {
            sr_guest_vram_write_prepare_bytes(dataBuf, (uint32_t)to_read);
            int loaded = s_storage.read(dir, fileName, (uint8_t *)SR_HOST(dataBuf),
                                        (uint32_t)to_read, &rd);
            sr_guest_vram_write_commit_bytes(dataBuf, rd);
            if (!loaded) {
                if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> file not found\n", dir);
                return ERR_LOAD_NOT_FOUND;
            }
        }
        MEM_W32(param + SDP_dataSize, rd);
        if (sdlog()) {
            const uint8_t *p = dataBuf ? (const uint8_t *)SR_HOST(dataBuf) : NULL;
            uint8_t b0 = (p && rd > 0) ? p[0] : 0;
            uint8_t b1 = (p && rd > 1) ? p[1] : 0;
            uint8_t b2 = (p && rd > 2) ? p[2] : 0;
            uint8_t b3 = (p && rd > 3) ? p[3] : 0;
            fprintf(stderr, "savedata: LOAD %s (%u bytes) -> buf=0x%08" PRIx32 " cap=%" PRIu32 " first=%02x%02x%02x%02x\n",
                    dir, (unsigned)rd, dataBuf, cap, b0, b1, b2, b3);
        }
    } else if (sdlog()) {
        fprintf(stderr, "savedata: LOAD %s (blank fileName: SFO/icons only)\n", dir);
    }
    /* copy the resolved save dir name back into the request (PPSSPP) */
    wr_fixed(param + SDP_saveName, save, 20);
    load_sfo_param(param, dir);            /* hand TITLE/SAVEDATA_TITLE/... back (LoadSFO) */
    load_filedata(dir, "ICON0.PNG", param + SDP_icon0);
    load_filedata(dir, "ICON1.PMF", param + SDP_icon1);
    load_filedata(dir, "PIC1.PNG", param + SDP_pic1);
    load_filedata(dir, "SND0.AT3", param + SDP_snd0);
    MEM_W32(param + SDP_bind, 1021);       /* PSP always responds this; unlocks some games */
    return 0;
}

/* DELETE family: remove one save directory and everything in it.
 *
 * Both destructive savedata paths now speak the host-neutral contained-delete
 * contract (vfs_contained.h) instead of a host primitive. No absolute pathname
 * is re-resolved here, and no host that lacks a containment backend gets an
 * unsafe by-name fallback -- it gets SR_CD_UNSUPPORTED_HOST and deletes
 * nothing. See vfs_contained.h for the per-backend anchoring proofs. */
static uint32_t do_delete(const char *game, const char *save) {
    char rel[SR_CD_REL_MAX];
    if (!save_rel(rel, sizeof(rel), game, save)) return ERR_DELETE_NO_DATA;
    sr_cd_root root;
    sr_cd_status st = sr_cd_root_open(ms_root(), &root);
    if (st == SR_CD_OK) {
        st = sr_cd_delete_dir_shallow(&root, rel);
        sr_cd_root_close(&root);
    }
    if (sdlog())
        fprintf(stderr, "savedata: DELETE %s [%s] -> %s\n", rel, sr_cd_backend_name(),
                sr_cd_status_name(st));
    return st == SR_CD_OK ? 0 : ERR_DELETE_NO_DATA;
}

/* ERASE/ERASESECURE: remove just the named data file inside the save dir (PPSSPP DeleteData). */
static uint32_t do_erase(uint32_t param, const char *game, const char *save) {
    char rel[SR_CD_REL_MAX], fileName[16];
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    if (!fileName[0] || !path_sanitize(fileName)) return ERR_RW_NO_DATA;
    if (!save_rel(rel, sizeof(rel), game, save)) return ERR_RW_NO_DATA;
    sr_cd_root root;
    sr_cd_status st = sr_cd_root_open(ms_root(), &root);
    if (st == SR_CD_OK) {
        /* A directory entry fails closed with SR_CD_IS_DIRECTORY: ERASE names a
         * file, and the backend proves that through the deletion primitive
         * itself rather than through a by-name type probe. */
        st = sr_cd_delete_leaf(&root, rel, fileName);
        sr_cd_root_close(&root);
    }
    if (sdlog())
        fprintf(stderr, "savedata: ERASE %s/%s [%s] -> %s\n", rel, fileName,
                sr_cd_backend_name(), sr_cd_status_name(st));
    return st == SR_CD_OK ? 0 : ERR_RW_NO_DATA;
}

/* LIST (11): fill idList with this game's save directories. */
static uint32_t do_list(uint32_t param, const char *game) {
    uint32_t idList = MEM_R32(param + SDP_idList);
    if (!idList) return 0;
    uint32_t maxCount = MEM_R32(idList + 0);
    uint32_t entries = MEM_R32(idList + 8);
    uint32_t count = 0;
    char root[PATH_MAX], path[PATH_MAX];
    snprintf(root, sizeof(root), "%s/PSP/SAVEDATA", ms_root());
#ifdef _WIN32
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)))
        return 0;
    if (!sr_vfs_dir_is_contained(root, canonical_root)) return 0;
#endif
    DIR *d = opendir(root);
    if (d) {
        size_t gl = strlen(game);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' || strncmp(de->d_name, game, gl) != 0) continue;
            /* A directory name is guest-influenced data at rest; the same
             * component rules apply before anything is opened or listed. */
            if (!path_sanitize(de->d_name)) continue;
            if (!path_join(path, sizeof(path), root, de->d_name)) continue;
#ifdef _WIN32
            if (!sr_vfs_dir_is_contained(path, canonical_root)) continue;
#endif
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (entries && count < maxCount) {
                uint32_t e = entries + count * 72u;       /* SceUtilitySavedataIdListEntry */
                MEM_W32(e + 0, 0x11FF);                   /* st_mode (directory) */
                put_psp_time(e + 4, st.st_ctime);
                put_psp_time(e + 20, st.st_atime);
                put_psp_time(e + 36, st.st_mtime);
                wr_fixed(e + 52, de->d_name + gl, 20);  /* saveName part only */
            }
            count++;
        }
        closedir(d);
    }
    if (count > maxCount) count = maxCount;
    MEM_W32(idList + 4, count);                           /* resultCount */
    if (sdlog()) fprintf(stderr, "savedata: LIST %s* -> %" PRIu32 " saves\n", game, count);
    return 0;
}

static int is_system_file(const char *n) {
    return !sd_stricmp(n, "PARAM.SFO") || !sd_stricmp(n, "ICON0.PNG") || !sd_stricmp(n, "ICON1.PMF") ||
           !sd_stricmp(n, "PIC1.PNG") || !sd_stricmp(n, "SND0.AT3");
}

/* FILES (12): list the files inside one save directory. */
static uint32_t do_files(uint32_t param, const char *game, const char *save) {
    uint32_t fl = MEM_R32(param + SDP_fileList);
    if (!fl) return 0;
    uint32_t maxSec = MEM_R32(fl + 0), maxNorm = MEM_R32(fl + 4), maxSys = MEM_R32(fl + 8);
    uint32_t pSec = MEM_R32(fl + 24), pNorm = MEM_R32(fl + 28), pSys = MEM_R32(fl + 32);
    uint32_t nSec = 0, nNorm = 0, nSys = 0;
    char path[PATH_MAX], dir[PATH_MAX];
    save_dir(dir, sizeof(dir), game, save);
#ifdef _WIN32
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)) ||
        !sr_vfs_dir_is_contained(dir, canonical_root)) {
        MEM_W32(fl + 12, 0); MEM_W32(fl + 16, 0); MEM_W32(fl + 20, 0);
        return ERR_LOAD_NO_DATA;
    }
#endif
    DIR *d = opendir(dir);
    if (!d) {
        MEM_W32(fl + 12, 0); MEM_W32(fl + 16, 0); MEM_W32(fl + 20, 0);
        return ERR_LOAD_NO_DATA;                  /* no such save: FILES_NO_DATA semantics */
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || savedata_is_stage_name(de->d_name)) continue;
        if (!path_sanitize(de->d_name)) continue;
        if (!path_join(path, sizeof(path), dir, de->d_name)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int sys = is_system_file(de->d_name);
        uint32_t *cnt = sys ? &nSys : &nNorm;
        uint32_t base = sys ? pSys : pNorm, cap = sys ? maxSys : maxNorm;
        for (int pass = 0; pass < (sys ? 1 : 2); pass++) {
            if (pass == 1) { cnt = &nSec; base = pSec; cap = maxSec; }   /* data files also "secure" */
            if (base && *cnt < cap) {
                uint32_t e = base + *cnt * 80u;            /* SceUtilitySavedataFileListEntry */
                MEM_W32(e + 0, 0x21FF);                    /* st_mode (file) */
                MEM_W32(e + 4, 0);
                MEM_W32(e + 8, (uint32_t)(uint64_t)st.st_size);
                MEM_W32(e + 12, (uint32_t)((uint64_t)st.st_size >> 32));
                put_psp_time(e + 16, st.st_ctime);
                put_psp_time(e + 32, st.st_atime);
                put_psp_time(e + 48, st.st_mtime);
                wr_fixed(e + 64, de->d_name, 16);
            }
            (*cnt)++;
        }
    }
    closedir(d);
    if (nSec > maxSec) nSec = maxSec;
    if (nNorm > maxNorm) nNorm = maxNorm;
    if (nSys > maxSys) nSys = maxSys;
    MEM_W32(fl + 12, nSec); MEM_W32(fl + 16, nNorm); MEM_W32(fl + 20, nSys);
    return 0;
}

/* The utility's size text: divide by 1024 through B, KB, MB and GB until the
 * value is below 1024, then "<n> <unit>" ("1 GB" on the PSP in getsize.expected).
 * Free space rounds down; needed and overwrite space round up. */
static void savedata_space_text(uint64_t bytes, int round_up, char *out, size_t cap) {
    static const char *const units[] = { "B", "KB", "MB", "GB" };
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (bytes < 1024u) {
            snprintf(out, cap, "%" PRIu64 " %s", bytes, units[i]);
            return;
        }
        bytes = round_up ? bytes / 1024u + (bytes % 1024u != 0u) : bytes / 1024u;
    }
    snprintf(out, cap, "%" PRIu64 " TB", bytes);
}

/* Free space of the host-backed Memory Stick in whole 32 KiB sectors. Every
 * free figure a game sees (sectors, KB, text) and the GETSIZE shortfall are
 * derived from this one sector-aligned byte count, so they always agree. */
typedef struct {
    uint32_t sectors;
    uint32_t kb;
    uint64_t bytes;
    char text[24];
} SavedataFree;

static int savedata_free_space(SavedataFree *f) {
    uint64_t host_free = 0;
    if (!f || !sr_hle_memstick_free_bytes(&host_free)) return 0;
    uint64_t sectors = host_free / CLUSTER;
    if (sectors > UINT32_MAX / (CLUSTER / 0x400u)) sectors = UINT32_MAX / (CLUSTER / 0x400u);
    f->sectors = (uint32_t)sectors;
    f->kb = f->sectors * (CLUSTER / 0x400u);
    f->bytes = sectors * CLUSTER;
    savedata_space_text(f->bytes, 0, f->text, sizeof(f->text));
    return 1;
}

static uint32_t do_sizes(uint32_t param, const char *game) {
    uint32_t msFree = MEM_R32(param + SDP_msFree);
    if (msFree) {
        SavedataFree fr;
        if (!savedata_free_space(&fr)) return ERR_SIZES_ACCESS_ERROR_UNMEASURED;
        MEM_W32(msFree + 0, CLUSTER);
        MEM_W32(msFree + 4, fr.sectors);
        MEM_W32(msFree + 8, fr.kb);
        wr_fixed(msFree + 12, fr.text, 8);
    }
    int sizes_no_data = 0;
    uint32_t msData = MEM_R32(param + SDP_msData);
    if (msData) {
        /* info block at +36: used clusters/KB of the named save (0 if absent) */
        char game2[16], save2[24], dir[PATH_MAX], path[PATH_MAX];
        rd_cstr(msData + 0, game2, 14);
        rd_cstr(msData + 16, save2, 21);
        save_dir(dir, sizeof(dir), game2[0] ? game2 : game, save2);
        if (dir_exists(dir)) {
            uint64_t used = 0;
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (savedata_is_stage_name(de->d_name)) continue;   /* issue #1387 */
                    if (!path_sanitize(de->d_name)) continue;
                    if (!path_join(path, sizeof(path), dir, de->d_name)) continue;
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                        used += ((uint64_t)st.st_size + CLUSTER - 1) / CLUSTER * CLUSTER;
                }
                closedir(d);
            }
            MEM_W32(msData + 36, (uint32_t)(used / CLUSTER));
            MEM_W32(msData + 40, (uint32_t)(used / 0x400));
            wr_fixed(msData + 44, "", 8);
            MEM_W32(msData + 52, (uint32_t)(used / 0x400));
            wr_fixed(msData + 56, "", 8);
        } else {
            /* PPSSPP SavedataParam::GetSizes: a SIZES query for a save that does NOT exist
             * zeroes the used-space block and returns SIZES_NO_DATA — that's how the game
             * learns "this is a brand-new save". Our old code always returned 0 ("it exists"),
             * so the game treated a freshly-created pilot as an overwrite of existing data and
             * skipped registering it into a profile slot (menu stayed USER:UNKNOWN). */
            MEM_W32(msData + 36, 0);
            MEM_W32(msData + 40, 0);
            wr_fixed(msData + 44, "", 8);
            MEM_W32(msData + 52, 0);
            wr_fixed(msData + 56, "", 8);
            sizes_no_data = 1;
        }
    }
    uint32_t ud = MEM_R32(param + SDP_usedData);
    if (ud) {
        uint32_t total = CLUSTER + CLUSTER;                 /* directory record + SFO */
        if (MEM_R8(param + SDP_fileName) != 0) {
            uint32_t ds = MEM_R32(param + SDP_dataSize);
            total += ((ds + CLUSTER - 1) / CLUSTER) * CLUSTER;
        }
        uint32_t sizes[4] = { MEM_R32(param + SDP_icon0 + 8), MEM_R32(param + SDP_icon1 + 8),
                              MEM_R32(param + SDP_pic1 + 8), MEM_R32(param + SDP_snd0 + 8) };
        for (int i = 0; i < 4; i++) total += ((sizes[i] + CLUSTER - 1) / CLUSTER) * CLUSTER;
        MEM_W32(ud + 0, total / CLUSTER);
        MEM_W32(ud + 4, total / 0x400);
        wr_fixed(ud + 8, "", 8);
        MEM_W32(ud + 16, total / 0x400);
        wr_fixed(ud + 20, "", 8);
    }
    return sizes_no_data ? ERR_SIZES_NO_DATA : 0;
}

static int validate_getsize_entries(uint32_t count, uint32_t entries, uint32_t *span) {
    if (!span || count > SAVEDATA_MAX_FILE_ENTRIES ||
        !sr_size_mul_ok(count, SAVEDATA_SIZE_ENTRY_BYTES, span)) {
        return 0;
    }
    if (count > 0 && (!entries || !sr_guest_span_readable(entries, *span))) return 0;
    return 1;
}

/* One sizeInfo entry (PspUtilitySavedataSizeEntry: u64 size, char name[16]). */
static int getsize_entry(uint32_t entries, uint32_t index, uint64_t *size, char *name, int name_cap) {
    uint32_t offset, entry;
    if (!size || !name || !sr_size_mul_ok(index, SAVEDATA_SIZE_ENTRY_BYTES, &offset) ||
        !sr_size_add_ok(entries, offset, &entry)) return 0;
    *size = MEM_R32(entry) | ((uint64_t)MEM_R32(entry + 4) << 32);
    rd_cstr(entry + 8, name, name_cap);
    return 1;
}

/* Bytes the named file already occupies in the save directory (0 when the save
 * or the file does not exist): the part of a write that replaces old data. */
static uint64_t getsize_existing_bytes(const char *dir, const char *name) {
    char path[PATH_MAX];
    struct stat st;
    if (!name[0] || savedata_is_stage_name(name) || !path_join(path, sizeof(path), dir, name))
        return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return 0;
    return (uint64_t)st.st_size;
}

static int getsize_sum_entries(uint32_t count, uint32_t entries, uint64_t per_entry_extra,
                               const char *dir, uint64_t *write_bytes, uint64_t *overwrite_bytes) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t size = 0;
        char name[17];
        if (!getsize_entry(entries, i, &size, name, (int)sizeof(name))) return 0;
        if (size > UINT64_MAX - per_entry_extra) return 0;
        size += per_entry_extra;
        if (size > UINT64_MAX - *write_bytes) return 0;
        *write_bytes += size;
        uint64_t existing = getsize_existing_bytes(dir, name);
        if (existing > UINT64_MAX - *overwrite_bytes) return 0;
        *overwrite_bytes += existing;
    }
    return 1;
}

/* KB for the 32-bit sizeInfo fields, rounded up. A shortfall beyond 4 TiB is
 * not representable there and saturates at the field's maximum. */
static uint32_t getsize_kb_round_up(uint64_t bytes) {
    uint64_t kb = bytes / 0x400u + (bytes % 0x400u != 0u);
    return kb > UINT32_MAX ? UINT32_MAX : (uint32_t)kb;
}

/* GETSIZE (22): free space, and how much MORE space a save of the listed files
 * needs. On a PSP (pspautotests tests/utility/savedata/getsize.expected,
 * revision 2ed7c9c5), a zero-filled sizeInfo with no entries for a save that
 * does not exist changes only sectorSize, freeSectors, freeKB and freeString,
 * and the result is 0: neededKB and overwriteKB are a shortfall, 0 when the
 * data fits, never the size of the data. The entry arithmetic follows PPSSPP's
 * SavedataParam::GetSize as a behaviour reference: each secure file adds 16
 * bytes, files already in the save count as overwritable, and the needed and
 * overwrite strings are written only when there is a shortfall. */
static uint32_t do_getsize(uint32_t param, const char *game, const char *save) {
    uint32_t si = MEM_R32(param + SDP_sizeInfo);
    if (!si) return 0;
    if (!sr_guest_span_readable(si, SAVEDATA_SIZE_INFO_BYTES) ||
        !sr_guest_span_writable(si, SAVEDATA_SIZE_INFO_BYTES)) return 0x80110381u;
    uint32_t nSec = MEM_R32(si + 0), nNorm = MEM_R32(si + 4);
    uint32_t pSec = MEM_R32(si + 8), pNorm = MEM_R32(si + 12);
    uint32_t secSpan = 0, normSpan = 0;
    if (!validate_getsize_entries(nSec, pSec, &secSpan) ||
        !validate_getsize_entries(nNorm, pNorm, &normSpan)) return 0x80110381u;
    (void)secSpan;
    (void)normSpan;
    char dir[PATH_MAX];
    save_dir(dir, sizeof(dir), game, save);
    uint64_t write_bytes = 0, overwrite_bytes = 0;
    if (!getsize_sum_entries(nSec, pSec, 0x10u, dir, &write_bytes, &overwrite_bytes) ||
        !getsize_sum_entries(nNorm, pNorm, 0u, dir, &write_bytes, &overwrite_bytes))
        return 0x80110381u;
    SavedataFree fr;
    if (!savedata_free_space(&fr)) return ERR_RW_NO_MEMSTICK_UNMEASURED;
    MEM_W32(si + 16, CLUSTER);                              /* sectorSize */
    MEM_W32(si + 20, fr.sectors);                           /* freeSectors */
    MEM_W32(si + 24, fr.kb);                                /* freeKB */
    wr_fixed(si + 28, fr.text, 8);                          /* freeString */
    if (overwrite_bytes >= write_bytes || write_bytes - overwrite_bytes < fr.bytes) {
        MEM_W32(si + 36, 0);                                /* neededKB */
        MEM_W32(si + 48, 0);                                /* overwriteKB */
        return 0;
    }
    char text[24];
    uint64_t needed = write_bytes - fr.bytes;
    MEM_W32(si + 36, getsize_kb_round_up(needed));
    savedata_space_text(needed, 1, text, sizeof(text));
    wr_fixed(si + 40, text, 8);                             /* neededString */
    uint64_t overwrite = needed > overwrite_bytes ? needed - overwrite_bytes : 0;
    MEM_W32(si + 48, getsize_kb_round_up(overwrite));
    savedata_space_text(overwrite, 1, text, sizeof(text));
    wr_fixed(si + 52, text, 8);                             /* overwriteString */
    return 0;
}

/* Resolve the effective saveName (PPSSPP GetSaveDirName): "<>" is a wildcard meaning "any
 * existing save"; the LIST modes carry a saveNameList the user would normally pick from in
 * the system UI -- headless, auto-select the first entry whose directory exists (falling
 * back to the first entry for a fresh LISTSAVE). */
static void resolve_save(uint32_t param, uint32_t mode, const char *game, char *save, int cap) {
    rd_cstr(param + SDP_saveName, save, cap);
    int wild = !strcmp(save, "<>");
    int isList = (mode == SD_LISTLOAD || mode == SD_LISTSAVE || mode == SD_LISTDELETE);
    uint32_t list = MEM_R32(param + SDP_saveNameList);
    if ((isList || wild) && list) {
        char ent[24], first[24] = "", dir[PATH_MAX];
        for (uint32_t i = 0; i < SAVEDATA_MAX_FILE_ENTRIES; i++) {
            rd_cstr(list + (uint32_t)i * 20, ent, 21);
            if (!ent[0]) break;
            if (!strcmp(ent, "<>")) continue;
            if (!first[0]) snprintf(first, sizeof(first), "%s", ent);
            save_dir(dir, sizeof(dir), game, ent);
            if (dir_exists(dir)) { snprintf(save, (size_t)cap, "%s", ent); return; }
        }
        if (mode == SD_LISTSAVE && first[0]) { snprintf(save, (size_t)cap, "%s", first); return; }
    }
    if (wild) {
        /* no list (or none existed): first existing dir matching <game>* */
        char root[PATH_MAX], path[PATH_MAX];
        snprintf(root, sizeof(root), "%s/PSP/SAVEDATA", ms_root());
        DIR *d = opendir(root);
        save[0] = 0;
        if (d) {
            struct dirent *de;
            size_t gl = strlen(game);
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, game, gl) != 0) continue;
                if (!path_sanitize(de->d_name)) continue;
                if (!path_join(path, sizeof(path), root, de->d_name)) continue;
                struct stat st;
                if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    snprintf(save, (size_t)cap, "%s", de->d_name + gl);
                    break;
                }
            }
            closedir(d);
        }
    }
}

uint32_t sr_savedata_execute(uint32_t param) {
    if (!param || !sr_guest_span_readable(param, 0x600u) || !sr_guest_span_writable(param, 0x600u)) return 0x80110381u;
    uint32_t mode = MEM_R32(param + SDP_mode);
    char game[16], save[24];
    rd_cstr(param + SDP_gameName, game, 14);
    resolve_save(param, mode, game, save, 21);
    if (sdlog()) fprintf(stderr, "savedata: mode=%" PRIu32 " game='%s' save='%s'\n", mode, game, save);
    switch (mode) {
        case SD_AUTOSAVE: case SD_SAVE: case SD_LISTSAVE:
        case SD_MAKEDATA: case SD_MAKEDATASECURE:
        case SD_WRITEDATA: case SD_WRITEDATASECURE: {
            uint32_t r = do_save(param, game, save);
            /* PPSSPP: MAKEDATA reports a full stick with the RW error code */
            if (r == 0x80110381u && (mode == SD_MAKEDATA || mode == SD_MAKEDATASECURE))
                r = ERR_RW_MS_FULL;
            return r;
        }
        case SD_AUTOLOAD: case SD_LOAD: case SD_LISTLOAD:
            return do_load(param, game, save);
        case SD_READDATA: case SD_READDATASECURE: {
            uint32_t r = do_load(param, game, save);             /* PPSSPP error remap */
            if (r == ERR_LOAD_NO_DATA) r = ERR_RW_NO_DATA;
            if (r == ERR_LOAD_NOT_FOUND) r = ERR_RW_NOT_FOUND;
            return r;
        }
        case SD_LISTDELETE: case SD_DELETE: case SD_AUTODELETE:
            return do_delete(game, save);
        case SD_DELETEDATA: {
            uint32_t r = do_delete(game, save);
            return r == ERR_DELETE_NO_DATA ? ERR_RW_NO_DATA : r;
        }
        case SD_ERASE: case SD_ERASESECURE:
            return do_erase(param, game, save);
        case SD_LIST:
            return do_list(param, game);
        case SD_FILES:
            return do_files(param, game, save);
        case SD_SIZES:
            return do_sizes(param, game);
        case SD_GETSIZE:
            return do_getsize(param, game, save);
        default:
            if (sdlog()) fprintf(stderr, "savedata: UNHANDLED mode=%" PRIu32 "\n", mode);
            return 0;
    }
}
