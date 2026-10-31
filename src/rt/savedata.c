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
 * follow PPSSPP's SceUtilitySavedataParam (Core/Dialog/SavedataParam.h), including the
 * abortStatus word that comes before msFree, idList and fileList.
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

/* Bookkeeping names this runtime keeps inside the Memory Stick: a SAVE's staged temporaries,
 * the previous versions it keeps until it commits, its transaction record and the root lock
 * (see "the SAVE transaction" below). They all start ".nakagawa-", compared without case
 * because Win32 names are case-insensitive. No member, save directory or listing ever has one,
 * and every one of them is at least 16 bytes, longer than any savedata fileName (rd_cstr caps
 * it at 15), so a SAVE can never name one. A guest name with that prefix is refused. */
#define SAVEDATA_RESERVED_PREFIX ".nakagawa-"
static int savedata_is_reserved_name(const char *name) {
    static const char prefix[] = SAVEDATA_RESERVED_PREFIX;
    if (!name) return 0;
    for (size_t i = 0; i + 1u < sizeof(prefix); i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c != (unsigned char)prefix[i]) return 0;
    }
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
 * presenting their own UI; PPSSPP routes them to the same Save/Load/Delete actions.
 *
 * LIMITATION (#1387): the *SECURE modes store and read the same unencrypted files as their
 * ordinary twins, and PARAM.SFO's SAVEDATA_PARAMS stays zero, which marks the save as not
 * encrypted. The game's key (offset 0x5dc) is not applied: savedata encryption needs keys only
 * the PSP's firmware holds, which this project never carries. So a save written here loads here
 * and in PPSSPP but not on a PSP, and a save a PSP (or PPSSPP, with a key) encrypted is refused
 * rather than handed back as ciphertext (savedata_encrypted). */
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

/* Failures this runtime reports by name, with PPSSPP's codes (Core/HLE/ErrorCodes.h, revision
 * c9e2afb6). UNMEASURED: no PSP capture shows which code firmware returns in these cases.
 * An I/O, recovery or lock failure is an access error, and only a full volume is reported as a
 * full Memory Stick: "full" makes a game ask the player to delete saves. */
#define ERR_SAVE_NOSPACE_UNMEASURED       0x80110383u
#define ERR_SAVE_ACCESS_ERROR_UNMEASURED  0x80110385u
#define ERR_SAVE_PARAM_UNMEASURED         0x80110388u
#define ERR_LOAD_ACCESS_ERROR_UNMEASURED  0x80110305u
#define ERR_DELETE_ACCESS_ERROR_UNMEASURED 0x80110345u
#define ERR_RW_BAD_PARAMS_UNMEASURED      0x80110328u
#define ERR_SIZES_PARAM_UNMEASURED        0x801103C8u
#define ERR_TYPE_UNMEASURED               0x80110300u   /* SAVEDATA_ERROR_TYPE: no such mode */
/* The common-block result of a dialog the person cancelled (SCE_UTILITY_DIALOG_RESULT_CANCEL,
 * as PPSSPP's PSPSaveDialog writes it on cancel). */
#define SAVEDATA_DIALOG_RESULT_CANCEL     1u

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

/* The save directory's name, gameName followed by saveName. It is the JOINED name that reaches
 * the host, so that is what the component rules judge: "NU" + "L" is the device alias "NUL",
 * while a saveName such as "AUX" after a gameName is an ordinary name. Each part only has to be
 * free of separators and dot segments, and gameName must not be empty. Bookkeeping names
 * (".nakagawa-...") are never a save directory. */
static int save_leaf(char *out, size_t cap, const char *game, const char *save) {
    if (!game || !*game || !sr_cd_component_is_generic(game, strlen(game))) return 0;
    if (save && *save && !sr_cd_component_is_generic(save, strlen(save))) return 0;
    int n = snprintf(out, cap, "%s%s", game, save ? save : "");
    return n > 0 && (size_t)n < cap && sr_cd_component_is_generic(out, (size_t)n) &&
           sr_vfs_is_safe_component(out, (size_t)n) && !savedata_is_reserved_name(out);
}

/* memstick/PSP/SAVEDATA/<gameName><saveName> — resolved through the shared
 * sr_ms0_resolve so savedata and ordinary sceIo* agree on one host path. Returns 0, with `out`
 * empty, for a name no save directory can have: the caller refuses the request by name. (It
 * used to fall back to one shared PSP/SAVEDATA/INVALID directory, so unrelated refused requests
 * saved over and loaded each other's data.) */
static int save_dir(char *out, int cap, const char *game, const char *save) {
    char leaf[64], guest[96];
    out[0] = '\0';
    if (!save_leaf(leaf, sizeof(leaf), game, save)) return 0;
    if (snprintf(guest, sizeof(guest), "PSP/SAVEDATA/%s", leaf) >= (int)sizeof(guest)) return 0;
    if (!sr_ms0_resolve(ms_root(), guest, out, (size_t)cap, '/')) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Root-RELATIVE form of the same location: "PSP/SAVEDATA/<gameName><saveName>".
 * The contained-delete seam anchors on the trusted root itself and walks this
 * relative path component by component, so destructive savedata operations
 * never hand a host a re-resolvable absolute pathname.
 *
 * Like save_dir this validates the CONCATENATION as one component (save_leaf):
 * "NU" and "L" are each a safe component but "NUL" is a device alias, and it is
 * the joined name that reaches the host. */
static int save_rel(char *out, size_t cap, const char *game, const char *save) {
    char leaf[64];
    if (!save_leaf(leaf, sizeof(leaf), game, save)) return 0;
    int n = snprintf(out, cap, "PSP/SAVEDATA/%s", leaf);
    return n > 0 && (size_t)n < cap;
}

static int sdlog(void) { static int v = -1; if (v < 0) v = getenv("SR_DLGLOG") ? 1 : 0; return v; }

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

/* One regular file's size and modification time (seconds since 1970-01-01 UTC), through the
 * same contained routes as host_read_file. A link, directory or special file is refused, so a
 * listing never reports an object outside the save directory. */
static int host_file_stat(const char *dir, const char *name, uint64_t *size, int64_t *mtime) {
    if (!path_sanitize(name)) return 0;
#ifdef _WIN32
    char path[PATH_MAX];
    wchar_t canonical_root[MAX_PATH * 2];
    if (!path_join(path, sizeof(path), dir, name) ||
        !ms_canonical_root(canonical_root, sizeof(canonical_root) / sizeof(wchar_t)))
        return 0;
    HANDLE h;
    if (!sr_vfs_open_contained_utf8(path, FILE_READ_ATTRIBUTES,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                    OPEN_EXISTING, canonical_root, &h))
        return 0;
    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(h, &info) &&
             !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    CloseHandle(h);
    if (!ok) return 0;
    ULARGE_INTEGER ft;
    ft.LowPart = info.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = info.ftLastWriteTime.dwHighDateTime;
    if (size) *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (mtime) *mtime = (int64_t)(ft.QuadPart / 10000000u) - 11644473600;
    return 1;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    char rel[SR_CD_REL_MAX];
    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;
    sr_cd_root root;
    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;
    FILE *f = NULL;
    sr_cd_status st = sr_cd_open_file(&root, rel, name, SR_CD_FILE_READ, &f);
    sr_cd_root_close(&root);
    if (st != SR_CD_OK || !f) return 0;
    struct stat info;
    int ok = fstat(fileno(f), &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) return 0;
    if (size) *size = (uint64_t)info.st_size;
    if (mtime) *mtime = (int64_t)info.st_mtime;
    return 1;
#else
    (void)dir; (void)size; (void)mtime;
    return 0;
#endif
}

/* A whole regular file read through ONE contained open, so the size checked against the guest's
 * buffer is the size of the bytes read. Returns 1 with *size the file's size and the bytes in
 * `data` when it fits `cap`; 2 (nothing read, *size the real size) when it does not fit; 0 when
 * it cannot be opened or a read comes back short. */
static int host_read_exact(const char *dir, const char *name, uint8_t *data, uint32_t cap,
                           uint32_t *size) {
    if (!path_sanitize(name)) return 0;
#ifdef _WIN32
    char path[PATH_MAX];
    wchar_t canonical_root[MAX_PATH * 2];
    if (!path_join(path, sizeof(path), dir, name) ||
        !ms_canonical_root(canonical_root, sizeof(canonical_root) / sizeof(wchar_t)))
        return 0;
    HANDLE h;
    if (!sr_vfs_open_contained_utf8(path, GENERIC_READ, FILE_ATTRIBUTE_NORMAL, OPEN_EXISTING,
                                    canonical_root, &h))
        return 0;
    LARGE_INTEGER sz;
    int ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart <= UINT32_MAX;
    int result = 0;
    if (ok) {
        *size = (uint32_t)sz.QuadPart;
        if (*size > cap) {
            result = 2;
        } else {
            uint32_t got = 0;
            while (ok && got < *size) {
                DWORD n = 0;
                ok = ReadFile(h, data + got, *size - got, &n, NULL) && n > 0;
                got += ok ? n : 0;
            }
            result = ok && got == *size ? 1 : 0;
        }
    }
    CloseHandle(h);
    return result;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    char rel[SR_CD_REL_MAX];
    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;
    sr_cd_root root;
    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;
    FILE *f = NULL;
    sr_cd_status st = sr_cd_open_file(&root, rel, name, SR_CD_FILE_READ, &f);
    sr_cd_root_close(&root);
    if (st != SR_CD_OK || !f) return 0;
    struct stat info;
    int result = 0;
    if (fstat(fileno(f), &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        (uint64_t)info.st_size <= UINT32_MAX) {
        *size = (uint32_t)info.st_size;
        if (*size > cap) result = 2;
        else result = (!*size || fread(data, 1, *size, f) == *size) && !ferror(f) ? 1 : 0;
    }
    if (fclose(f) != 0 && result == 1) result = 0;
    return result;
#else
    (void)dir; (void)data; (void)cap; (void)size;
    return 0;
#endif
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void free_names(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* The entry names of one host directory, sorted bytewise, without "." and ".." and without
 * bookkeeping names. A PSP lists a FAT directory in its own entry order, which a host folder
 * cannot reproduce, and host readdir order differs between file systems (#1387). Returns 0, and
 * no names, when the directory cannot be opened or read in full. */
static int savedata_sorted_names(const char *dir, char ***names_out, size_t *count_out) {
    *names_out = NULL;
    *count_out = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    char **names = NULL;
    size_t count = 0, cap = 0;
    int ok = 1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") ||
            savedata_is_reserved_name(de->d_name))
            continue;
        if (count == cap) {
            size_t grown_cap = cap ? cap * 2u : 16u;
            char **grown = (char **)realloc(names, grown_cap * sizeof(*names));
            if (!grown) { ok = 0; break; }
            names = grown;
            cap = grown_cap;
        }
        size_t len = strlen(de->d_name);
        char *copy = (char *)malloc(len + 1u);
        if (!copy) { ok = 0; break; }
        memcpy(copy, de->d_name, len + 1u);
        names[count++] = copy;
    }
    closedir(d);
    if (!ok) {
        free_names(names, count);
        return 0;
    }
    if (count) qsort(names, count, sizeof(*names), cmp_names);
    *names_out = names;
    *count_out = count;
    return 1;
}

/* A save directory's time (see "save times"): its PARAM.SFO's modification time, else its own. */
static int64_t savedata_save_time(const char *dir) {
    int64_t when = 0;
    if (host_file_stat(dir, "PARAM.SFO", NULL, &when)) return when;
    struct stat st;
    return stat(dir, &st) == 0 ? (int64_t)st.st_mtime : 0;
}

static int dir_exists(const char *dir);     /* below, with the modes */
static int is_system_file(const char *n);  /* below, with FILES */

/* ---- the SAVE transaction (#1387) ---------------------------------------------------------
 *
 * One SAVE replaces up to six member files of one save directory -- the data file, PARAM.SFO
 * and the ICON0, ICON1, PIC1 and SND0 blobs -- and is all or nothing: a SAVE that fails leaves
 * every member as it was, and a process stopped at any instant leaves a directory the next
 * savedata operation puts back to either the whole previous save or the whole new one, never
 * a mix. The rule throughout: nothing is deleted or overwritten unless its content proves it
 * is a superseded copy.
 *
 * Names, all reserved (savedata_is_reserved_name) and hidden from every listing:
 *   S_i  .nakagawa-stage-<i>      the staged new bytes of slot i
 *   P(m) .nakagawa-prev-<m>       the previous version of member m while the SAVE runs
 *   J    .nakagawa-journal        the transaction record before the commit
 *   C    .nakagawa-commit         the same record after it: the commit point
 *   K    .nakagawa-kept-<n>-<m>   a file kept out of the way (below); never restored or deleted
 * The record lists each slot's member name (as it is spelled on disk), whether a previous
 * version existed (only "no such file" counts as absent; any other error fails the SAVE
 * first), and the size and FNV-1a 64 of the previous and the new content, with a hash over
 * the whole record.
 *
 * SAVE, under the root lock (savedata_lock):
 *   1. recover the directory, which must end with no record, P or S left under a live name;
 *   2. stage each member in S_i (exclusive create, write, guest-clock time, flush);
 *   3. read each existing member for the record;
 *   4. write J (exclusive), flush it and the directory;
 *   5. per slot, rename m -> P(m) if it existed, then S_i -> m, both refusing to replace;
 *   6. flush the directory, then rename J -> C, the commit, and flush again;
 *   7. clean up through recovery: P copies and C go.
 * Any failure closes what is open without deleting anything and runs recovery, whose verdict
 * decides the result: a C it finds means the SAVE committed.
 *
 * Recovery (savedata_recover) reads the disk, never memory:
 *   - J valid: roll back. A slot that had a previous version gets it back from P when the
 *     member holds the new bytes or is missing; one that had none loses the member only if it
 *     holds exactly the new bytes. A member with any other content is not ours and stays.
 *   - C valid: roll forward, never back. Only a lost rename is completed (S still holds the
 *     new bytes); a slot that cannot reach the new bytes is left exactly as found and logged.
 *     P is deleted only where the member holds the new bytes.
 *   - No record but a P: the record was lost (a file system without ordered metadata, or a
 *     file removed from outside), so the directory fails closed and every file is kept.
 *   - A file that may not be deleted but must leave its live name (a duplicate the host refuses
 *     to delete, a stage with foreign content, the previous version an automatic list overwrite
 *     replaced) is renamed to a K name and logged with its path.
 *   - Every slot is classified before any is changed; every step is a no-op when its target
 *     state already holds, so recovery can be repeated.
 *
 * Crash safety needs atomic renames and flushable, ordered metadata (NTFS, ReFS, ext4, xfs,
 * btrfs). FAT32/exFAT, network shares and 9p/drvfs do not order metadata: in-process failures
 * are still all or nothing there, and the content checks keep recovery from destroying data,
 * but a power loss can leave a directory that recovery refuses (every file kept) instead of
 * one it repairs.
 *
 * The Win32 renames go through the file's own verified handle (SetFileInformationByHandle),
 * never a path-based MoveFileExW, which would resolve the names again. MOVEFILE_WRITE_THROUGH
 * only changes a move Windows performs as copy and delete, which a same-directory rename never
 * is; durability comes from the explicit flushes of each file and of the directory. */
#define SAVEDATA_STAGE_PREFIX SAVEDATA_RESERVED_PREFIX "stage-"
#define SAVEDATA_PREV_PREFIX  SAVEDATA_RESERVED_PREFIX "prev-"
#define SAVEDATA_KEPT_PREFIX  SAVEDATA_RESERVED_PREFIX "kept-"
#define SAVEDATA_JOURNAL_NAME SAVEDATA_RESERVED_PREFIX "journal"
#define SAVEDATA_COMMIT_NAME  SAVEDATA_RESERVED_PREFIX "commit"
#define SAVEDATA_LOCK_NAME    SAVEDATA_RESERVED_PREFIX "savedata-lock"
#define SAVEDATA_STAGE_MAX 6u      /* data file, PARAM.SFO, ICON0, ICON1, PIC1, SND0 */
#define SAVEDATA_KEPT_MAX 1000u

/* Test builds only: a fault seam over every host operation that changes a save directory
 * (#1387). Each such operation counts one step; ONCE fails step k without doing it, FROM fails
 * k and every later step (a process that stopped, or a disk that stopped taking writes), and
 * EFFECT does step k and then reports failure (a rename that happened but returned an error). */
#ifdef SR_HLE_THREAD_SELFTEST
enum { SD_FAULT_OFF = 0, SD_FAULT_ONCE = 1, SD_FAULT_FROM = 2, SD_FAULT_EFFECT = 3 };
static int s_sd_fault_mode, s_sd_fault_at, s_sd_fault_ops;
void sr_savedata_test_fault(int mode, int at) {
    s_sd_fault_mode = mode;
    s_sd_fault_at = at;
    s_sd_fault_ops = 0;
}
int sr_savedata_test_fault_ops(void) { return s_sd_fault_ops; }
/* 0: go ahead; 1: fail without doing it; 2: do it, then fail. */
static int sd_fault(void) {
    if (s_sd_fault_mode == SD_FAULT_OFF) return 0;
    const int k = ++s_sd_fault_ops;
    if (s_sd_fault_mode == SD_FAULT_ONCE) return k == s_sd_fault_at;
    if (s_sd_fault_mode == SD_FAULT_FROM) return k >= s_sd_fault_at;
    return k == s_sd_fault_at ? 2 : 0;
}
#else
#define sd_fault() 0
#endif

static uint64_t sd_fnv1a64(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
#define SD_FNV_OFFSET 0xcbf29ce484222325ull

/* A file's content, as recovery compares it: absent, or a regular file of `size` bytes whose
 * FNV-1a 64 is `hash`. disk_name is its name as the directory spells it (Win32 names are
 * case-insensitive, so "data.bin" answers to "DATA.BIN"). */
typedef struct {
    int present;
    uint64_t size, hash;
    char disk_name[16];
} SdContent;

static int sd_is(const SdContent *c, uint64_t size, uint64_t hash) {
    return c->present && c->size == size && c->hash == hash;
}

/* One save directory, bound once for a whole operation. */
typedef struct SavedataDir {
    char dir[PATH_MAX];
    int open;
#ifdef _WIN32
    wchar_t canonical_root[MAX_PATH * 2];
    wchar_t final_dir[MAX_PATH * 2];     /* the directory's own verified final path */
#elif defined(SR_CD_BACKEND_POSIX_AT)
    sr_cd_root root;
    char rel[SR_CD_REL_MAX];
#endif
} SavedataDir;

#ifdef _WIN32
#ifndef FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000010
#endif
/* Open `name` in the directory (NULL: the directory itself) and verify the object is inside the
 * Memory Stick root. *err receives the Win32 error of a failed open, so "no such file" can be
 * told apart from every other failure. */
static int sd_win_open(const SavedataDir *d, const char *name, DWORD access, DWORD flags,
                       DWORD disposition, DWORD share, HANDLE *out, DWORD *err) {
    char path[PATH_MAX];
    *out = NULL;
    if (err) *err = ERROR_INVALID_NAME;
    if (name ? !path_join(path, sizeof(path), d->dir, name)
             : snprintf(path, sizeof(path), "%s", d->dir) >= (int)sizeof(path))
        return 0;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (need <= 0) return 0;
    wchar_t *wpath = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wpath) return 0;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, need);
    for (wchar_t *p = wpath; *p; p++) if (*p == L'/') *p = L'\\';
    HANDLE h = CreateFileW(wpath, access, share, NULL, disposition, flags, NULL);
    DWORD open_err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = open_err;
        return 0;
    }
    if (!sr_vfs_handle_is_contained(h, d->canonical_root)) {
        CloseHandle(h);
        if (err) *err = ERROR_ACCESS_DENIED;
        return 0;
    }
    *out = h;
    return 1;
}

static int sd_win_not_found(DWORD err) {
    return err == ERROR_FILE_NOT_FOUND;
}

/* The final path of an open handle, and the position of its last separator. */
static int sd_win_final(HANDLE h, wchar_t *out, size_t cap, size_t *slash) {
    DWORD len = GetFinalPathNameByHandleW(h, out, (DWORD)cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= cap) return 0;
    wchar_t *s = wcsrchr(out, L'\\');
    if (!s) return 0;
    *slash = (size_t)(s - out);
    return 1;
}
#endif

static void sd_dir_close(SavedataDir *d) {
#if !defined(_WIN32) && defined(SR_CD_BACKEND_POSIX_AT)
    if (d->open) sr_cd_root_close(&d->root);
#endif
    d->open = 0;
}

static int sd_dir_open(SavedataDir *d, const char *dir) {
    memset(d, 0, sizeof(*d));
    if (snprintf(d->dir, sizeof(d->dir), "%s", dir) >= (int)sizeof(d->dir)) return 0;
#ifdef _WIN32
    if (!ms_canonical_root(d->canonical_root, sizeof(d->canonical_root) / sizeof(wchar_t)))
        return 0;
    HANDLE h;
    size_t slash = 0;
    if (!sd_win_open(d, NULL, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS, OPEN_EXISTING,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &h, NULL))
        return 0;
    int ok = sd_win_final(h, d->final_dir, sizeof(d->final_dir) / sizeof(wchar_t), &slash);
    CloseHandle(h);
    if (!ok) return 0;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    if (!savedata_root_relative(dir, d->rel, sizeof(d->rel))) return 0;
    if (sr_cd_root_open(ms_root(), &d->root) != SR_CD_OK) return 0;
#else
    return 0;   /* an unsupported host has no contained write route */
#endif
    d->open = 1;
    return 1;
}

/* Read `name` whole and fill *c. Returns 1 when the answer is known (present or absent), 0 when
 * it is not: an I/O error, or something other than a regular file (a directory or a link) at
 * that name. */
static int sd_content(SavedataDir *d, const char *name, SdContent *c) {
    memset(c, 0, sizeof(*c));
    if (!path_sanitize(name)) return 0;
    uint8_t buf[16384];
    uint64_t h = SD_FNV_OFFSET, total = 0;
#ifdef _WIN32
    HANDLE fh;
    DWORD err = 0;
    if (!sd_win_open(d, name, GENERIC_READ | FILE_READ_ATTRIBUTES,
                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, OPEN_EXISTING,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &fh, &err))
        return sd_win_not_found(err);
    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(fh, &info) &&
             !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    while (ok) {
        DWORD n = 0;
        if (!ReadFile(fh, buf, (DWORD)sizeof(buf), &n, NULL)) { ok = 0; break; }
        if (!n) break;
        h = sd_fnv1a64(h, buf, n);
        total += n;
    }
    wchar_t final_path[MAX_PATH * 2];
    size_t slash = 0;
    snprintf(c->disk_name, sizeof(c->disk_name), "%s", name);
    if (ok && sd_win_final(fh, final_path, sizeof(final_path) / sizeof(wchar_t), &slash)) {
        char leaf[64];
        int n = WideCharToMultiByte(CP_UTF8, 0, final_path + slash + 1, -1, leaf, (int)sizeof(leaf),
                                    NULL, NULL);
        if (n > 1 && (size_t)n <= sizeof(c->disk_name) && sd_stricmp(leaf, name) == 0)
            memcpy(c->disk_name, leaf, (size_t)n);
    }
    CloseHandle(fh);
    if (!ok) return 0;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    FILE *f = NULL;
    sr_cd_status st = sr_cd_open_file(&d->root, d->rel, name, SR_CD_FILE_READ, &f);
    if (st == SR_CD_NOT_FOUND) return 1;
    if (st != SR_CD_OK || !f) return 0;
    struct stat info;
    int ok = fstat(fileno(f), &info) == 0 && S_ISREG(info.st_mode);
    while (ok) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        h = sd_fnv1a64(h, buf, n);
        total += n;
        if (n < sizeof(buf)) { ok = !ferror(f); break; }
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) return 0;
    snprintf(c->disk_name, sizeof(c->disk_name), "%s", name);
#else
    (void)d; (void)buf; (void)h; (void)total;
    return 0;
#endif
    c->present = 1;
    c->size = total;
    c->hash = h;
    return 1;
}

/* Create `name` exclusively, write `n` bytes, give it the guest-clock time `mtime` (best effort:
 * a time the host refuses is logged, never a failure) and flush it. *nospace reports a full
 * volume. One fault step. */
static int sd_write_new(SavedataDir *d, const char *name, const uint8_t *data, uint32_t n,
                        int64_t mtime, int *nospace) {
    const int fault = sd_fault();
    if (fault == 1) return 0;
    if ((n > 0 && !data) || !path_sanitize(name)) return 0;
    int ok, time_ok;
#ifdef _WIN32
    HANDLE h;
    DWORD err = 0;
    if (!sd_win_open(d, name, GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, CREATE_NEW,
                     FILE_SHARE_READ | FILE_SHARE_DELETE, &h, &err))
        return 0;
    DWORD written = 0;
    ok = !n || (WriteFile(h, data, (DWORD)n, &written, NULL) && written == (DWORD)n);
    if (!ok && nospace) {
        DWORD e = GetLastError();
        *nospace = e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL;
    }
    ULARGE_INTEGER t;
    t.QuadPart = (uint64_t)(mtime + 11644473600) * 10000000ull;
    FILETIME ft = { t.LowPart, t.HighPart };
    time_ok = SetFileTime(h, NULL, &ft, &ft) != 0;
    if (ok) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
#elif defined(SR_CD_BACKEND_POSIX_AT)
    FILE *f = NULL;
    if (sr_cd_open_file(&d->root, d->rel, name, SR_CD_FILE_READ_WRITE_NEW, &f) != SR_CD_OK || !f)
        return 0;
    ok = !n || fwrite(data, 1, n, f) == n;
    if (ok && fflush(f) != 0) ok = 0;
    if (!ok && nospace) *nospace = errno == ENOSPC;
    struct timespec times[2] = { { (time_t)mtime, 0 }, { (time_t)mtime, 0 } };
    time_ok = futimens(fileno(f), times) == 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
#else
    (void)d; (void)data; (void)n; (void)mtime; (void)nospace; (void)time_ok;
    return 0;
#endif
    if (!time_ok)
        fprintf(stderr, "savedata: %s/%s keeps the host's time: the guest-clock time was refused\n",
                d->dir, name);
    return ok && fault != 2;
}

/* Rename `from` to `to` in the directory. replace == 0 refuses an existing destination. Win32:
 * through the source's own handle, pinned to the object at that name (a link or directory there
 * is refused) and checked to be in this very directory; POSIX: sr_cd_rename_file_ex. One fault
 * step. */
static int sd_rename(SavedataDir *d, const char *from, const char *to, int replace) {
    const int fault = sd_fault();
    if (fault == 1) return 0;
    if (!path_sanitize(from) || !path_sanitize(to)) return 0;
    int ok = 0;
#ifdef _WIN32
    HANDLE h;
    if (!sd_win_open(d, from, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, OPEN_EXISTING,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &h, NULL))
        return 0;
    BY_HANDLE_FILE_INFORMATION info;
    wchar_t final_path[MAX_PATH * 2];
    size_t slash = 0;
    if (GetFileInformationByHandle(h, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        sd_win_final(h, final_path, sizeof(final_path) / sizeof(wchar_t), &slash) &&
        wcslen(d->final_dir) == slash && _wcsnicmp(final_path, d->final_dir, slash) == 0) {
        const size_t cap = sizeof(final_path) / sizeof(final_path[0]);
        int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, to, -1, NULL, 0);
        if (need > 1 && slash + 1u + (size_t)need <= cap) {
            size_t name_chars = slash + 1u + (size_t)(need - 1);
            size_t total = offsetof(FILE_RENAME_INFO, FileName) + (name_chars + 1u) * sizeof(wchar_t);
            FILE_RENAME_INFO *ri = (FILE_RENAME_INFO *)malloc(total);
            if (ri) {
                memset(ri, 0, total);
                ri->FileNameLength = (DWORD)(name_chars * sizeof(wchar_t));
                memcpy(ri->FileName, final_path, (slash + 1u) * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, to, -1,
                                    ri->FileName + slash + 1u, need);
#if defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && defined(NTDDI_VERSION) && \
    NTDDI_VERSION >= 0x0A000002
                /* POSIX semantics let the rename go ahead while a guest sceIoOpen descriptor
                 * (which shares delete access) holds the file open, as renameat does. Hosts
                 * without the class or the flag get the classic rename. */
                ri->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS |
                            (replace ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS : 0u);
                ok = SetFileInformationByHandle(h, FileRenameInfoEx, ri, (DWORD)total);
                if (!ok) {
                    DWORD err = GetLastError();
                    if (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED ||
                        err == ERROR_CALL_NOT_IMPLEMENTED || err == ERROR_INVALID_FUNCTION) {
                        ri->Flags = 0;
                        ri->ReplaceIfExists = replace ? TRUE : FALSE;
                        ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)total);
                    }
                }
#else
                ri->ReplaceIfExists = replace ? TRUE : FALSE;
                ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)total);
#endif
                free(ri);
            }
        }
    }
    CloseHandle(h);
#elif defined(SR_CD_BACKEND_POSIX_AT)
    char a[SR_CD_REL_MAX], b[SR_CD_REL_MAX];
    ok = snprintf(a, sizeof(a), "%s/%s", d->rel, from) < (int)sizeof(a) &&
         snprintf(b, sizeof(b), "%s/%s", d->rel, to) < (int)sizeof(b) &&
         sr_cd_rename_file_ex(&d->root, a, b, replace) == SR_CD_OK;
#else
    (void)d; (void)replace;
#endif
    return ok && fault != 2;
}

/* Delete one file (never a directory). 1 when it is gone, or was never there. A read-only file
 * is deleted too where the host allows it (FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE): a
 * member a game made read-only must not leave its previous version stuck. One fault step. */
static int sd_delete(SavedataDir *d, const char *name) {
    const int fault = sd_fault();
    if (fault == 1) return 0;
    if (!path_sanitize(name)) return 0;
    int ok = 0;
#ifdef _WIN32
    HANDLE h;
    DWORD err = 0;
    if (!sd_win_open(d, name, DELETE | FILE_READ_ATTRIBUTES,
                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, OPEN_EXISTING,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &h, &err))
        return sd_win_not_found(err) && fault != 2;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
#if defined(FILE_DISPOSITION_FLAG_POSIX_SEMANTICS)  /* the Ex class (Windows 10 1709+) */
        FILE_DISPOSITION_INFO_EX ex;
        ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                   FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        ok = SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof(ex));
        if (!ok) {
            ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
            ok = SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof(ex));
        }
#endif
        if (!ok) ok = sr_vfs_dispose_by_handle(h);
    }
    CloseHandle(h);
#elif defined(SR_CD_BACKEND_POSIX_AT)
    sr_cd_status st = sr_cd_delete_leaf(&d->root, d->rel, name);
    ok = st == SR_CD_OK || st == SR_CD_NOT_FOUND;
#else
    (void)d;
#endif
    return ok && fault != 2;
}

/* Ask the host to make the directory's entries durable. One fault step. */
static int sd_flush_dir(SavedataDir *d) {
    const int fault = sd_fault();
    if (fault == 1) return 0;
    int ok = 0;
#ifdef _WIN32
    HANDLE h;
    /* FlushFileBuffers needs write access to the directory handle; FILE_ADD_FILE is enough
     * (on NTFS a read-only handle is refused with ERROR_ACCESS_DENIED). */
    if (sd_win_open(d, NULL, FILE_ADD_FILE | FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS,
                    OPEN_EXISTING, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &h, NULL)) {
        ok = FlushFileBuffers(h) != 0;
        CloseHandle(h);
    }
#elif defined(SR_CD_BACKEND_POSIX_AT)
    ok = sr_cd_sync_dir(&d->root, d->rel) == SR_CD_OK;
#else
    (void)d;
#endif
    return ok && fault != 2;
}

static int stage_name(char *out, size_t cap, unsigned index) {
    int n = snprintf(out, cap, SAVEDATA_STAGE_PREFIX "%u", index);
    return n > 0 && (size_t)n < cap && path_sanitize(out);
}

static int prev_name(char *out, size_t cap, const char *member) {
    int n = snprintf(out, cap, SAVEDATA_PREV_PREFIX "%s", member);
    return n > 0 && (size_t)n < cap && path_sanitize(out);
}

/* Move `name` to the first free K name for `member`, keeping it. */
static int sd_keep(SavedataDir *d, const char *name, const char *member, const char *why) {
    char kept[64];
    for (unsigned seq = 0; seq < SAVEDATA_KEPT_MAX; seq++) {
        SdContent c;
        int n = snprintf(kept, sizeof(kept), SAVEDATA_KEPT_PREFIX "%u-%s", seq, member);
        if (n <= 0 || (size_t)n >= sizeof(kept) || !path_sanitize(kept)) return 0;
        if (!sd_content(d, kept, &c)) return 0;
        if (c.present) continue;
        if (!sd_rename(d, name, kept, 0)) break;
        fprintf(stderr, "savedata: kept %s/%s as %s (%s)\n", d->dir, name, kept, why);
        return 1;
    }
    fprintf(stderr, "savedata: could not keep %s/%s out of the way (%s)\n", d->dir, name, why);
    return 0;
}

/* Delete a proven duplicate; if the host refuses, keep it under a K name instead. */
static int sd_drop(SavedataDir *d, const char *name, const char *member) {
    return sd_delete(d, name) || sd_keep(d, name, member, "the host refused to delete it");
}

/* ---- the transaction record ----------------------------------------------------------------- */
#define SD_RECORD_MAGIC   0x4a534b4eu   /* "NKSJ" */
#define SD_RECORD_VERSION 1u
#define SD_RECORD_FLAG_KEEP_PREVIOUS 1u /* keep the replaced version (an automatic list overwrite) */
#define SD_RECORD_HEAD    16u           /* magic, version, flags, slot count */
#define SD_RECORD_SLOT    56u           /* name[16], prev, pad[7], old size/hash, new size/hash */
#define SD_RECORD_MAX     (SD_RECORD_HEAD + SD_RECORD_SLOT * SAVEDATA_STAGE_MAX + 8u)

typedef struct {
    char name[16];
    uint8_t prev;                       /* a regular file held the member name */
    uint64_t old_size, old_hash, new_size, new_hash;
} SdSlot;

typedef struct {
    uint32_t flags;
    unsigned n;
    SdSlot s[SAVEDATA_STAGE_MAX];
} SdRecord;

static void sd_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void sd_put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t sd_get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint64_t sd_get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint32_t sd_record_encode(const SdRecord *r, uint8_t *out) {
    memset(out, 0, SD_RECORD_MAX);
    sd_put32(out + 0, SD_RECORD_MAGIC);
    sd_put32(out + 4, SD_RECORD_VERSION);
    sd_put32(out + 8, r->flags);
    sd_put32(out + 12, r->n);
    for (unsigned i = 0; i < r->n; i++) {
        uint8_t *s = out + SD_RECORD_HEAD + SD_RECORD_SLOT * i;
        memcpy(s, r->s[i].name, sizeof(r->s[i].name));
        s[16] = r->s[i].prev;
        sd_put64(s + 24, r->s[i].old_size);
        sd_put64(s + 32, r->s[i].old_hash);
        sd_put64(s + 40, r->s[i].new_size);
        sd_put64(s + 48, r->s[i].new_hash);
    }
    uint32_t body = SD_RECORD_HEAD + SD_RECORD_SLOT * r->n;
    sd_put64(out + body, sd_fnv1a64(SD_FNV_OFFSET, out, body));
    return body + 8u;
}

enum { SD_REC_ABSENT, SD_REC_VALID, SD_REC_INVALID, SD_REC_ERROR };

/* Read and validate a record: exact length, magic, version, slot count, every name a safe,
 * unreserved component unique under case folding, and the hash. A record whose hash holds but
 * whose version is unknown is INVALID too (it is not ours to interpret). */
static int sd_record_read(SavedataDir *d, const char *name, SdRecord *r) {
    SdContent c;
    uint8_t b[SD_RECORD_MAX];
    uint32_t n = 0;
    memset(r, 0, sizeof(*r));
    if (!sd_content(d, name, &c)) return SD_REC_ERROR;
    if (!c.present) return SD_REC_ABSENT;
    if (c.size > SD_RECORD_MAX) return SD_REC_INVALID;
    if (host_read_exact(d->dir, name, b, (uint32_t)sizeof(b), &n) != 1) return SD_REC_ERROR;
    if (n < SD_RECORD_HEAD + 8u || sd_get32(b) != SD_RECORD_MAGIC) return SD_REC_INVALID;
    r->flags = sd_get32(b + 8);
    r->n = sd_get32(b + 12);
    if (r->n < 1u || r->n > SAVEDATA_STAGE_MAX || n != SD_RECORD_HEAD + SD_RECORD_SLOT * r->n + 8u ||
        sd_get64(b + n - 8u) != sd_fnv1a64(SD_FNV_OFFSET, b, n - 8u) ||
        sd_get32(b + 4) != SD_RECORD_VERSION)
        return SD_REC_INVALID;
    for (unsigned i = 0; i < r->n; i++) {
        const uint8_t *s = b + SD_RECORD_HEAD + SD_RECORD_SLOT * i;
        memcpy(r->s[i].name, s, sizeof(r->s[i].name));
        r->s[i].name[sizeof(r->s[i].name) - 1u] = '\0';
        r->s[i].prev = s[16];
        r->s[i].old_size = sd_get64(s + 24);
        r->s[i].old_hash = sd_get64(s + 32);
        r->s[i].new_size = sd_get64(s + 40);
        r->s[i].new_hash = sd_get64(s + 48);
        if (!path_sanitize(r->s[i].name) || savedata_is_reserved_name(r->s[i].name) ||
            r->s[i].prev > 1u)
            return SD_REC_INVALID;
        for (unsigned j = 0; j < i; j++)
            if (sd_stricmp(r->s[j].name, r->s[i].name) == 0) return SD_REC_INVALID;
    }
    return SD_REC_VALID;
}

/* ---- recovery ------------------------------------------------------------------------------- */
enum { SD_RECOVER_CLEAN, SD_RECOVER_ROLLED_BACK, SD_RECOVER_COMMITTED, SD_RECOVER_FAILED };

/* The live bookkeeping names in the directory: stage and prev temporaries (K names are kept on
 * purpose and do not count). Returns -1 when the directory cannot be read. */
static int sd_scan(SavedataDir *d, char names[][40], int cap, int *has_prev) {
    DIR *dd = opendir(d->dir);
    int n = 0;
    *has_prev = 0;
    if (!dd) return -1;
    struct dirent *de;
    while ((de = readdir(dd)) != NULL) {
        const int prev = !strncmp(de->d_name, SAVEDATA_PREV_PREFIX, sizeof(SAVEDATA_PREV_PREFIX) - 1u);
        const int stage = !strncmp(de->d_name, SAVEDATA_STAGE_PREFIX, sizeof(SAVEDATA_STAGE_PREFIX) - 1u);
        if (!prev && !stage) continue;
        if (prev) *has_prev = 1;
        const size_t len = strlen(de->d_name);
        if (n < cap && len < 40u) memcpy(names[n++], de->d_name, len + 1u);
        else n = cap + 1;               /* more than the scan holds: the caller fails closed */
    }
    closedir(dd);
    return n;
}

static void sd_log_slot(const char *what, const SavedataDir *d, const SdSlot *s,
                        const SdContent *m, const SdContent *p, const SdContent *st) {
    fprintf(stderr, "savedata: %s %s/%s: member %s, previous copy %s, staged copy %s\n", what,
            d->dir, s->name,
            !m->present ? "absent" : sd_is(m, s->new_size, s->new_hash) ? "new" :
                sd_is(m, s->old_size, s->old_hash) && s->prev ? "old" : "other",
            !p->present ? "absent" : sd_is(p, s->old_size, s->old_hash) ? "old" : "other",
            !st->present ? "absent" : sd_is(st, s->new_size, s->new_hash) ? "new" : "other");
}

/* Classify every slot of a record: its member, its P and its S. */
static int sd_classify(SavedataDir *d, const SdRecord *r, SdContent m[], SdContent p[],
                       SdContent s[]) {
    for (unsigned i = 0; i < r->n; i++) {
        char pn[40], sn[40];
        if (!prev_name(pn, sizeof(pn), r->s[i].name) || !stage_name(sn, sizeof(sn), i) ||
            !sd_content(d, r->s[i].name, &m[i]) || !sd_content(d, pn, &p[i]) ||
            !sd_content(d, sn, &s[i])) {
            fprintf(stderr, "savedata: recovery of %s cannot read slot %s\n", d->dir, r->s[i].name);
            return 0;
        }
    }
    return 1;
}

/* Roll back an uncommitted SAVE (J valid). Returns 0 when a slot's previous version cannot be
 * proven present (fail closed, nothing changed), 1 when every member is back; *leftover is set
 * when a duplicate could not be removed (the record then stays, so a later recovery retries). */
static int sd_roll_back(SavedataDir *d, const SdRecord *r, int *leftover) {
    SdContent m[SAVEDATA_STAGE_MAX], p[SAVEDATA_STAGE_MAX], s[SAVEDATA_STAGE_MAX];
    if (!sd_classify(d, r, m, p, s)) return 0;
    enum { KEEP, RESTORE, REMOVE } act[SAVEDATA_STAGE_MAX];
    for (unsigned i = 0; i < r->n; i++) {
        const SdSlot *sl = &r->s[i];
        act[i] = KEEP;
        if (sl->prev) {
            if (sd_is(&m[i], sl->old_size, sl->old_hash)) continue;
            if (sd_is(&p[i], sl->old_size, sl->old_hash) &&
                (!m[i].present || sd_is(&m[i], sl->new_size, sl->new_hash))) {
                act[i] = RESTORE;
                continue;
            }
            sd_log_slot("rollback refused for", d, sl, &m[i], &p[i], &s[i]);
            return 0;
        }
        if (sd_is(&m[i], sl->new_size, sl->new_hash)) act[i] = REMOVE;
        else if (m[i].present) sd_log_slot("rollback keeps a file it did not write:", d, sl, &m[i], &p[i], &s[i]);
    }
    for (unsigned i = 0; i < r->n; i++) {
        const SdSlot *sl = &r->s[i];
        char pn[40], sn[40];
        (void)prev_name(pn, sizeof(pn), sl->name);
        (void)stage_name(sn, sizeof(sn), i);
        if (act[i] == RESTORE) {
            if (!sd_rename(d, pn, sl->name, m[i].present)) return 0;
            p[i].present = 0;
        } else if (act[i] == REMOVE) {
            if (!sd_drop(d, sl->name, sl->name)) return 0;
        }
        if (p[i].present &&
            !(sd_is(&p[i], sl->old_size, sl->old_hash) ? sd_drop(d, pn, sl->name)
                                                       : sd_keep(d, pn, sl->name, "an unproven previous copy")))
            *leftover = 1;
        if (s[i].present &&
            !(sd_is(&s[i], sl->new_size, sl->new_hash) ? sd_drop(d, sn, sl->name)
                                                       : sd_keep(d, sn, sl->name, "a staged file it did not write")))
            *leftover = 1;
    }
    return 1;
}

/* Finish a committed SAVE (C valid), never undoing it: complete each lost rename, then delete
 * P only where the member holds the new bytes (keep it under a K name when the record asks to
 * keep the previous version). Every slot is checked first: when any slot can reach its new bytes
 * neither in its member nor in its stage (a file system that lost a rename, or a member changed
 * from outside), nothing is touched and the record, every copy and every stage stay where they
 * are, so the save is never left partial with no record to repair it. */
static int sd_roll_forward(SavedataDir *d, const SdRecord *r, int *leftover) {
    SdContent m[SAVEDATA_STAGE_MAX], p[SAVEDATA_STAGE_MAX], s[SAVEDATA_STAGE_MAX];
    const int keep_previous = (r->flags & SD_RECORD_FLAG_KEEP_PREVIOUS) != 0;
    if (!sd_classify(d, r, m, p, s)) return 0;
    for (unsigned i = 0; i < r->n; i++) {
        const SdSlot *sl = &r->s[i];
        if (sd_is(&m[i], sl->new_size, sl->new_hash) || sd_is(&s[i], sl->new_size, sl->new_hash))
            continue;
        sd_log_slot("committed save cannot reach the new version of", d, sl, &m[i], &p[i], &s[i]);
        fprintf(stderr, "savedata: %s fails closed: its commit record and every copy are kept\n",
                d->dir);
        return 0;
    }
    for (unsigned i = 0; i < r->n; i++) {
        const SdSlot *sl = &r->s[i];
        char pn[40], sn[40];
        (void)prev_name(pn, sizeof(pn), sl->name);
        (void)stage_name(sn, sizeof(sn), i);
        int done = sd_is(&m[i], sl->new_size, sl->new_hash);
        if (!done && sd_is(&s[i], sl->new_size, sl->new_hash)) {
            /* A rename the crash lost: move whatever holds the member name aside, then publish. */
            if (m[i].present) {
                if (!p[i].present) {
                    if (!sd_rename(d, sl->name, pn, 0)) return 0;
                    p[i] = m[i];
                } else if (!sd_keep(d, sl->name, sl->name, "the file a committed save replaces")) {
                    return 0;
                }
            }
            if (!sd_rename(d, sn, sl->name, 0)) return 0;
            s[i].present = 0;
            done = 1;
        }
        if (p[i].present) {
            int ok = done && sd_is(&p[i], sl->old_size, sl->old_hash) && !keep_previous
                ? sd_drop(d, pn, sl->name)
                : sd_keep(d, pn, sl->name, keep_previous ? "the version an automatic list overwrite replaced"
                                                         : "a previous copy it cannot prove superseded");
            if (!ok) *leftover = 1;
        }
        if (s[i].present &&
            !(sd_is(&s[i], sl->new_size, sl->new_hash) ? sd_drop(d, sn, sl->name)
                                                       : sd_keep(d, sn, sl->name, "a staged file it did not write")))
            *leftover = 1;
    }
    return 1;
}

/* Put one save directory back to a whole save (see "the SAVE transaction"). require_empty: the
 * caller is about to SAVE, so no record, P or S may remain under a live name. */
static int sd_recover(SavedataDir *d, int require_empty) {
    SdRecord jr, cr;
    int leftover = 0, result = SD_RECOVER_CLEAN;
    const int jst = sd_record_read(d, SAVEDATA_JOURNAL_NAME, &jr);
    const int cst = sd_record_read(d, SAVEDATA_COMMIT_NAME, &cr);
    if (jst == SD_REC_ERROR || cst == SD_REC_ERROR) {
        fprintf(stderr, "savedata: recovery of %s cannot read its transaction record\n", d->dir);
        return SD_RECOVER_FAILED;
    }
    if (cst == SD_REC_INVALID) {
        fprintf(stderr, "savedata: %s has a damaged commit record; every file is kept\n", d->dir);
        return SD_RECOVER_FAILED;
    }
    if (jst == SD_REC_VALID) {
        if (!sd_roll_back(d, &jr, &leftover)) return SD_RECOVER_FAILED;
        if (!leftover && !sd_flush_dir(d)) leftover = 1;
        if (!leftover && !sd_drop(d, SAVEDATA_JOURNAL_NAME, "journal")) leftover = 1;
        result = SD_RECOVER_ROLLED_BACK;
        fprintf(stderr, "savedata: %s: an unfinished save was rolled back; the previous save stands\n",
                d->dir);
    } else if (jst == SD_REC_INVALID) {
        /* Renames start only after J is whole on disk, so a damaged J with no P means nothing
         * was moved: its stage files are uncommitted bytes. With a P, fail closed. */
        char names[16][40];
        int has_prev = 0, n = sd_scan(d, names, 16, &has_prev);
        if (n < 0 || n > 16 || has_prev) {
            fprintf(stderr, "savedata: %s has a damaged journal beside a previous copy; every file "
                    "is kept\n", d->dir);
            return SD_RECOVER_FAILED;
        }
        for (int i = 0; i < n; i++) if (!sd_delete(d, names[i])) leftover = 1;
        if (!sd_drop(d, SAVEDATA_JOURNAL_NAME, "journal")) leftover = 1;
    }
    if (cst == SD_REC_VALID) {
        if (!sd_roll_forward(d, &cr, &leftover)) return SD_RECOVER_FAILED;
        if (!leftover && !sd_flush_dir(d)) leftover = 1;
        if (!leftover && !sd_drop(d, SAVEDATA_COMMIT_NAME, "commit")) leftover = 1;
        if (!leftover) (void)sd_flush_dir(d);
        result = SD_RECOVER_COMMITTED;
    }
    if (jst == SD_REC_ABSENT && cst == SD_REC_ABSENT) {
        char names[16][40];
        int has_prev = 0, n = sd_scan(d, names, 16, &has_prev);
        if (n < 0 || n > 16) return SD_RECOVER_FAILED;
        for (int i = 0; i < n; i++) {
            if (strncmp(names[i], SAVEDATA_PREV_PREFIX, sizeof(SAVEDATA_PREV_PREFIX) - 1u) != 0)
                continue;
            /* A P with no record: only a lost record leaves one. A P that equals its member is
             * a duplicate and is kept aside; any other P means a save may be split, so every
             * file stays where it is. */
            const char *member = names[i] + sizeof(SAVEDATA_PREV_PREFIX) - 1u;
            SdContent pc, mc;
            if (!path_sanitize(member) || savedata_is_reserved_name(member) ||
                !sd_content(d, names[i], &pc) || !sd_content(d, member, &mc) || !mc.present ||
                mc.size != pc.size || mc.hash != pc.hash) {
                fprintf(stderr, "savedata: %s holds %s with no transaction record; every file is "
                        "kept\n", d->dir, names[i]);
                return SD_RECOVER_FAILED;
            }
            if (!sd_keep(d, names[i], member, "a duplicate left with no record")) leftover = 1;
        }
        for (int i = 0; i < n; i++)    /* stage files with no record were never committed */
            if (!strncmp(names[i], SAVEDATA_STAGE_PREFIX, sizeof(SAVEDATA_STAGE_PREFIX) - 1u) &&
                !sd_delete(d, names[i]))
                leftover = 1;
    }
    if (require_empty) {
        SdContent j, c;
        char names[16][40];
        int has_prev = 0, n = sd_scan(d, names, 16, &has_prev);
        if (n != 0 || !sd_content(d, SAVEDATA_JOURNAL_NAME, &j) || j.present ||
            !sd_content(d, SAVEDATA_COMMIT_NAME, &c) || c.present) {
            fprintf(stderr, "savedata: %s still holds bookkeeping files a SAVE would collide "
                    "with; the SAVE is refused and every file is kept\n", d->dir);
            return SD_RECOVER_FAILED;
        }
    }
    return result;
}

/* Recover the save directory `dir` for a read or a delete. 1 when it holds a whole save (or no
 * transaction), 0 when it failed closed. A directory that does not exist has nothing to do. */
static int savedata_recover_dir(const char *dir, int require_empty) {
    SavedataDir d;
    if (!dir_exists(dir)) return 1;
    if (!sd_dir_open(&d, dir)) return 0;
    const int r = sd_recover(&d, require_empty);
    sd_dir_close(&d);
    return r != SD_RECOVER_FAILED;
}

static int savedata_recover(const char *dir) { return savedata_recover_dir(dir, 0); }

/* As savedata_recover, and no record, P or S may remain afterwards (before a partial delete). */
static int savedata_recover_settled(const char *dir) { return savedata_recover_dir(dir, 1); }

/* ---- the root lock --------------------------------------------------------------------------
 * One savedata operation at a time per Memory Stick root, across processes too: two players
 * sharing a root must never run one save directory's recovery against the other's live SAVE.
 * Taken once per top-level operation (sr_savedata_execute_sized) and held for all of it.
 * Win32: PSP/SAVEDATA/.nakagawa-savedata-lock opened with no sharing and deleted when closed.
 * POSIX: flock on the PSP/SAVEDATA directory itself (sr_cd_lock_dir). */
#ifdef _WIN32
static HANDLE s_sd_lock = NULL;
#else
static int s_sd_lock_fd = -1;
#endif
static int s_sd_lock_depth;

enum { SD_LOCK_HELD = 1, SD_LOCK_NONE = 0, SD_LOCK_BUSY = -1 };

/* create: make PSP/SAVEDATA first when it is missing (a SAVE); otherwise a missing PSP/SAVEDATA
 * means there is no save data at all and nothing to lock (SD_LOCK_NONE). */
static int savedata_lock(int create) {
    if (s_sd_lock_depth > 0) { s_sd_lock_depth++; return SD_LOCK_HELD; }
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/PSP/SAVEDATA", ms_root());
    if (!dir_exists(root)) {
        if (!create) return SD_LOCK_NONE;
        if (!mkdirs(root)) return SD_LOCK_BUSY;
    }
#ifdef _WIN32
    char path[PATH_MAX];
    wchar_t canonical_root[MAX_PATH * 2];
    if (!path_join(path, sizeof(path), root, SAVEDATA_LOCK_NAME) ||
        !ms_canonical_root(canonical_root, sizeof(canonical_root) / sizeof(wchar_t)))
        return SD_LOCK_BUSY;
    SavedataDir d;
    memset(&d, 0, sizeof(d));
    snprintf(d.dir, sizeof(d.dir), "%s", root);
    memcpy(d.canonical_root, canonical_root, sizeof(canonical_root));
    HANDLE h;
    DWORD err = 0;
    if (!sd_win_open(&d, SAVEDATA_LOCK_NAME, GENERIC_READ | GENERIC_WRITE | DELETE,
                     FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, OPEN_ALWAYS, 0, &h, &err)) {
        fprintf(stderr, "savedata: the Memory Stick's save data is in use by another savedata "
                "operation (%s, error %lu)\n", path, (unsigned long)err);
        return SD_LOCK_BUSY;
    }
    s_sd_lock = h;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    sr_cd_root r;
    int fd = -1;
    if (sr_cd_root_open(ms_root(), &r) != SR_CD_OK) return SD_LOCK_BUSY;
    sr_cd_status st = sr_cd_lock_dir(&r, "PSP/SAVEDATA", &fd);
    sr_cd_root_close(&r);
    if (st == SR_CD_UNSUPPORTED_HOST) {
        /* The file system has no locks: one process still works, but two sharing this root
         * are not kept apart. Said once, not refused: refusing would stop every save. */
        static int said;
        if (!said++)
            fprintf(stderr, "savedata: %s offers no file locks; two programs sharing this "
                    "Memory Stick folder are not kept from saving at the same time\n", root);
        return SD_LOCK_NONE;
    }
    if (st != SR_CD_OK) {
        fprintf(stderr, "savedata: the Memory Stick's save data is in use by another savedata "
                "operation (%s: %s)\n", root, sr_cd_status_name(st));
        return SD_LOCK_BUSY;
    }
    s_sd_lock_fd = fd;
#else
    return SD_LOCK_BUSY;
#endif
    s_sd_lock_depth = 1;
    return SD_LOCK_HELD;
}

static void savedata_unlock(int held) {
    if (held != SD_LOCK_HELD || s_sd_lock_depth <= 0 || --s_sd_lock_depth > 0) return;
#ifdef _WIN32
    if (s_sd_lock) CloseHandle(s_sd_lock);
    s_sd_lock = NULL;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    sr_cd_unlock_dir(s_sd_lock_fd);
    s_sd_lock_fd = -1;
#endif
}

/* Storage boundary: PSP serialization/crypto can be inserted by replacing these operations;
 * dialog and guest-memory code never depends on the host filesystem representation. */
typedef struct SavedataStorageOps {
    int (*prepare)(const char *dir);
    int (*read)(const char *dir, const char *name, uint8_t *data, uint32_t cap, uint32_t *size);
} SavedataStorageOps;

static const SavedataStorageOps s_storage = { mkdirs, host_read_file };

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

/* The PARAM.SFO a SAVE writes, built in memory (the caller frees it); the SAVE transaction
 * stages it like every other member. */
static uint8_t *sfo_build(const char *title, const char *saveTitle, const char *detail,
                          const char *saveDir, uint32_t parental, const char *fileName,
                          uint32_t *size_out) {
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
        if (e[i].len > e[i].maxlen) return NULL;
        keyOff[i] = keySize; keySize += (uint32_t)strlen(e[i].key) + 1;
        dataOff[i] = dataSize; dataSize += (e[i].maxlen + 3) & ~3u;
    }
    uint32_t keyStart = 20 + 16u * (uint32_t)n;
    uint32_t dataStart = (keyStart + keySize + 3) & ~3u;
    uint32_t totalAlloc = dataStart + dataSize;

    uint8_t *buf = (uint8_t *)calloc(1, totalAlloc);
    if (!buf) return NULL;
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
    *size_out = totalAlloc;
    return buf;
}

/* ---- save times (#1387) -------------------------------------------------------------------
 * A listing is the same on every host and every run for the same files:
 *  - names come back sorted bytewise (savedata_sorted_names), not in host readdir order;
 *  - a save's time is its PARAM.SFO's modification time, else its directory's, as PPSSPP's
 *    SavedataParam::GetSaveInfo takes it ("like the savedata manager does"); a file's time is
 *    its own modification time;
 *  - the created, accessed and modified fields all carry that one time. Host st_ctime means
 *    "inode changed" on POSIX and "created" on Win32, and st_atime moves whenever a host reads
 *    the file, so either would make a listing depend on the host and on earlier reads;
 *  - the time is shown in the console's time zone and daylight-saving setting, the ones the
 *    RTC's local time uses, never the host process's zone; whole seconds.
 * And a SAVE stamps what it writes with the guest's own clock (sr_savedata_guest_rtc_tick), so
 * the time a game reads back agrees with sceRtc even when guest time runs apart from host time.
 * UNMEASURED on a PSP: which times its LIST reports, and its FAT order. */
uint64_t sr_savedata_guest_rtc_tick(void);          /* src/rt/hle.c: microseconds since 0001 */
int32_t sr_savedata_console_offset_minutes(void);   /* src/rt/hle.c: zone + daylight saving */
#define SAVEDATA_RTC_UNIX_EPOCH_TICK 62135596800000000ull   /* 1970-01-01 as an RTC tick */

/* The guest wall clock in whole seconds since 1970-01-01 UTC (0 before it). */
static int64_t savedata_guest_unix_seconds(void) {
    uint64_t tick = sr_savedata_guest_rtc_tick();
    return tick > SAVEDATA_RTC_UNIX_EPOCH_TICK
        ? (int64_t)((tick - SAVEDATA_RTC_UNIX_EPOCH_TICK) / 1000000u) : 0;
}

/* ScePspDateTime (16 bytes: u16 year, month, day, hour, minute, second; u32 microsecond) for
 * an instant in seconds since 1970-01-01 UTC, in the console's local time. The civil date is
 * computed here (days-from-civil inverse), not by the host's localtime, which applies the host
 * process's zone. */
static void put_psp_time(uint32_t addr, int64_t unix_seconds) {
    int64_t t = unix_seconds + (int64_t)sr_savedata_console_offset_minutes() * 60;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t secs = t - days * 86400;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = yoe + era * 400 + (month <= 2);
    if (year < 0 || year > 0xffff) year = 0, month = 0, day = 0, secs = 0;
    MEM_W16(addr + 0, (uint16_t)year);
    MEM_W16(addr + 2, (uint16_t)month); MEM_W16(addr + 4, (uint16_t)day);
    MEM_W16(addr + 6, (uint16_t)(secs / 3600)); MEM_W16(addr + 8, (uint16_t)(secs / 60 % 60));
    MEM_W16(addr + 10, (uint16_t)(secs % 60)); MEM_W32(addr + 12, 0);
}

/* The three ScePspDateTime fields of a listing entry (created, accessed, modified), all one
 * time (see above). */
static void put_psp_times(uint32_t addr, int64_t unix_seconds) {
    put_psp_time(addr + 0, unix_seconds);
    put_psp_time(addr + 16, unix_seconds);
    put_psp_time(addr + 32, unix_seconds);
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

/* A save's PARAM.SFO, read whole through the storage boundary so it gets exactly the same
 * containment verification as guest data files. Returns the bytes (the caller frees them) with
 * the index table already checked against the file size, or NULL when there is no usable SFO.
 * The index table is a file envelope of its own: it is rejected before any entry is read, so
 * a forged count cannot make a parser walk a truncated table. */
static uint8_t *sfo_read(const char *dir, uint32_t *size_out) {
    uint32_t n = 0;
    if (!s_storage.read(dir, "PARAM.SFO", NULL, 0, &n)) return NULL;
    if (n <= 20 || n > (1 << 20)) return NULL;
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) return NULL;
    uint32_t actual_read = 0;
    uint32_t keyStart, dataStart, cnt;
    if (!s_storage.read(dir, "PARAM.SFO", b, n, &actual_read) || actual_read != n ||
        memcmp(b, "\0PSF", 4) != 0) {
        free(b);
        return NULL;
    }
    memcpy(&keyStart, b + 8, 4); memcpy(&dataStart, b + 12, 4); memcpy(&cnt, b + 16, 4);
    if (keyStart > n || dataStart > n || cnt > (n - 20u) / 16u) {
        free(b);
        return NULL;
    }
    *size_out = n;
    return b;
}

/* Find `key` in an SFO sfo_read returned. */
static int sfo_find(const uint8_t *b, uint32_t n, const char *key, uint16_t *fmt_out,
                    const uint8_t **val_out, uint32_t *len_out) {
    uint32_t keyStart, dataStart, cnt;
    memcpy(&keyStart, b + 8, 4); memcpy(&dataStart, b + 12, 4); memcpy(&cnt, b + 16, 4);
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
        size_t keyRemain = (size_t)((uint64_t)n - keyPos), keyLen = 0;
        while (keyLen < keyRemain && keyBytes[keyLen] != '\0') keyLen++;
        if (keyLen == keyRemain || strcmp((const char *)keyBytes, key) != 0) continue;
        *fmt_out = fmt;
        *val_out = b + (size_t)dataPos;
        *len_out = len;
        return 1;
    }
    return 0;
}

/* The save's encryption mode: the first byte of PARAM.SFO's SAVEDATA_PARAMS, which is 0 for an
 * unencrypted save and 0x01, 0x21 or 0x41 for one the PSP encrypted (PPSSPP
 * SavedataParam::GetSaveCryptMode, revision c9e2afb6: "save created in PPSSPP and not encrypted
 * has '0' in SAVEDATA_PARAMS"). No SFO at all, or a well-formed one without that entry, reads
 * as unencrypted, as there. Returns 0 when the mode is not known: a PARAM.SFO is there but cannot
 * be read or parsed (a link, a directory, a truncated or damaged file), which proves nothing
 * about the data beside it. */
static int savedata_crypt_mode(const char *dir, uint8_t *mode) {
    SavedataDir d;
    SdContent c;
    uint32_t n = 0, len = 0;
    uint16_t fmt = 0;
    const uint8_t *val = NULL;
    *mode = 0;
    if (!sd_dir_open(&d, dir)) return 0;
    const int known = sd_content(&d, "PARAM.SFO", &c);
    sd_dir_close(&d);
    if (!known) return 0;
    if (!c.present) return 1;
    uint8_t *b = sfo_read(dir, &n);
    if (!b) return 0;
    if (sfo_find(b, n, "SAVEDATA_PARAMS", &fmt, &val, &len) && len > 0) *mode = val[0];
    free(b);
    return 1;
}

/* A save this runtime cannot decrypt is never handed back as data, and never replaced or
 * deleted either: it may be the only copy of a save the player brought from a PSP. A save whose
 * PARAM.SFO cannot be read is treated the same way, since it may be one. Logged on every
 * refusal. */
static int savedata_encrypted(const char *dir, const char *what) {
    uint8_t mode = 0;
    if (!savedata_crypt_mode(dir, &mode)) {
        fprintf(stderr, "savedata: %s refused: %s has a PARAM.SFO that cannot be read, so the save "
                "may be encrypted; it is left exactly as it is\n", what, dir);
        return 1;
    }
    if (!mode) return 0;
    fprintf(stderr, "savedata: %s refused: %s is an encrypted save (SAVEDATA_PARAMS mode 0x%02x); "
            "decrypting it needs keys only the PSP's firmware holds, which this runtime does not "
            "carry, so it is left exactly as it is\n", what, dir, (unsigned)mode);
    return 1;
}

static void load_sfo_param(uint32_t param, const char *dir) {
    uint32_t n = 0;
    uint8_t *b = sfo_read(dir, &n);
    if (!b) return;
    uint32_t keyStart, dataStart, cnt;
    memcpy(&keyStart, b + 8, 4); memcpy(&dataStart, b + 12, 4); memcpy(&cnt, b + 16, 4);
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

/* One member a SAVE writes: its name and the guest (or built) bytes. */
typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
} SaveMember;

/* A PspUtilitySavedataFileData blob as a member. An absent blob is no member. */
static void save_blob(SaveMember *m, unsigned *n, const char *name, uint32_t fd) {
    uint32_t buf = MEM_R32(fd + 0), sz = MEM_R32(fd + 8);
    if (!buf || !sz) return;
    m[*n].name = name;
    m[*n].data = (const uint8_t *)SR_HOST(buf);
    m[*n].size = sz;
    (*n)++;
}

/* SAVE (and AUTOSAVE, LISTSAVE, MAKEDATA, WRITEDATA): one transaction over the members (see
 * "the SAVE transaction"). keep_previous: an automatically confirmed list overwrite keeps the
 * version it replaces under K names (resolve_save). */
static uint32_t do_save(uint32_t param, const char *game, const char *save, int keep_previous) {
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
    /* A data file named like a system member or a bookkeeping file would be replaced by that
     * member in the same SAVE, so the game's bytes would be lost while the SAVE succeeded. */
    if (fileName[0] && (is_system_file(fileName) || savedata_is_reserved_name(fileName))) {
        fprintf(stderr, "savedata: SAVE refused: the data file name '%s' is reserved for the "
                "save's own files\n", fileName);
        return ERR_SAVE_PARAM_UNMEASURED;
    }
    if (!save_dir(dir, sizeof(dir), game, save)) {
        fprintf(stderr, "savedata: SAVE refused: '%s%s' cannot name a save directory\n", game, save);
        return ERR_SAVE_PARAM_UNMEASURED;
    }

    const int existed = dir_exists(dir);
    if (!s_storage.prepare(dir)) return ERR_SAVE_ACCESS_ERROR_UNMEASURED;
    SavedataDir d;
    if (!sd_dir_open(&d, dir)) return ERR_SAVE_ACCESS_ERROR_UNMEASURED;
    if (sd_recover(&d, 1) == SD_RECOVER_FAILED) {
        sd_dir_close(&d);
        return ERR_SAVE_ACCESS_ERROR_UNMEASURED;
    }
    if (savedata_encrypted(dir, "SAVE")) {
        sd_dir_close(&d);
        return ERR_SAVE_ACCESS_ERROR_UNMEASURED;
    }

    SaveMember mem[SAVEDATA_STAGE_MAX];
    unsigned n = 0;
    uint32_t sfo_size = 0;
    rd_cstr(param + SDP_sfoTitle, title, sizeof(title));
    rd_cstr(param + SDP_sfoSaveTitle, saveTitle, sizeof(saveTitle));
    rd_cstr(param + SDP_sfoDetail, detail, sizeof(detail));
    snprintf(saveDir, sizeof(saveDir), "%s%s", game, save);
    uint8_t *sfo = sfo_build(title, saveTitle, detail, saveDir, MEM_R8(param + SDP_sfoParental),
                             fileName, &sfo_size);
    if (!sfo) {
        sd_dir_close(&d);
        return ERR_SAVE_ACCESS_ERROR_UNMEASURED;
    }
    if (fileName[0] && dataBuf && dataSize) {
        mem[n].name = fileName;
        mem[n].data = (const uint8_t *)SR_HOST(dataBuf);
        mem[n].size = dataSize;
        n++;
    }
    mem[n].name = "PARAM.SFO";
    mem[n].data = sfo;
    mem[n].size = sfo_size;
    n++;
    save_blob(mem, &n, "ICON0.PNG", param + SDP_icon0);
    save_blob(mem, &n, "ICON1.PMF", param + SDP_icon1);
    save_blob(mem, &n, "PIC1.PNG", param + SDP_pic1);
    save_blob(mem, &n, "SND0.AT3", param + SDP_snd0);

    SdRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.n = n;
    rec.flags = keep_previous ? SD_RECORD_FLAG_KEEP_PREVIOUS : 0u;
    const int64_t when = savedata_guest_unix_seconds();
    int nospace = 0, committed = 0, ok = 1;
    char sn[40], pn[40];
    /* 2. Stage every member. */
    for (unsigned i = 0; ok && i < n; i++) {
        ok = stage_name(sn, sizeof(sn), i) &&
             sd_write_new(&d, sn, mem[i].data, mem[i].size, when, &nospace);
        rec.s[i].new_size = mem[i].size;
        rec.s[i].new_hash = sd_fnv1a64(SD_FNV_OFFSET, mem[i].data, mem[i].size);
    }
    free(sfo);
    /* 3. What each member holds now. Only "no such file" counts as absent. */
    for (unsigned i = 0; ok && i < n; i++) {
        SdContent c;
        ok = sd_content(&d, mem[i].name, &c);
        snprintf(rec.s[i].name, sizeof(rec.s[i].name), "%s", ok && c.present ? c.disk_name : mem[i].name);
        rec.s[i].prev = (uint8_t)(ok && c.present);
        rec.s[i].old_size = c.size;
        rec.s[i].old_hash = c.hash;
        if (!ok) fprintf(stderr, "savedata: SAVE %s: %s is not a readable regular file\n", dir, mem[i].name);
    }
    /* 4-6. Journal, publish, commit. */
    if (ok) {
        uint8_t b[SD_RECORD_MAX];
        const uint32_t len = sd_record_encode(&rec, b);
        ok = sd_write_new(&d, SAVEDATA_JOURNAL_NAME, b, len, when, &nospace) && sd_flush_dir(&d);
    }
    for (unsigned i = 0; ok && i < n; i++) {
        ok = stage_name(sn, sizeof(sn), i) && prev_name(pn, sizeof(pn), rec.s[i].name) &&
             (!rec.s[i].prev || sd_rename(&d, rec.s[i].name, pn, 0)) &&
             sd_rename(&d, sn, rec.s[i].name, 0);
    }
    if (ok) ok = sd_flush_dir(&d);
    if (ok) committed = sd_rename(&d, SAVEDATA_JOURNAL_NAME, SAVEDATA_COMMIT_NAME, 0);
    if (committed && !sd_flush_dir(&d))
        fprintf(stderr, "savedata: SAVE %s committed, but the host could not confirm it is on disk\n", dir);
    /* 7. Clean up, or undo, from what is on disk. */
    const int r = sd_recover(&d, 0);
    sd_dir_close(&d);
    if (committed || r == SD_RECOVER_COMMITTED) {
        if (!committed)
            fprintf(stderr, "savedata: SAVE %s: the commit was reported as failed but took effect\n", dir);
        if (sdlog()) fprintf(stderr, "savedata: SAVE %s/%s (%" PRIu32 " bytes)\n", dir, fileName, dataSize);
        return 0;
    }
    if (!existed) {
        /* A first SAVE that failed leaves no empty directory that LIST would show as a save. */
        char rel[SR_CD_REL_MAX];
        sr_cd_root root;
        if (save_rel(rel, sizeof(rel), game, save) && sr_cd_root_open(ms_root(), &root) == SR_CD_OK) {
            (void)sr_cd_remove_dir_empty(&root, rel);
            sr_cd_root_close(&root);
        }
    }
    fprintf(stderr, "savedata: SAVE %s failed (%s); the previous save is kept\n", dir,
            nospace ? "the volume is full" : r == SD_RECOVER_FAILED ? "recovery is incomplete"
                                                                    : "a host operation failed");
    return nospace ? ERR_SAVE_NOSPACE_UNMEASURED : ERR_SAVE_ACCESS_ERROR_UNMEASURED;
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

    if (!save_dir(dir, sizeof(dir), game, save) || !dir_exists(dir)) {
        if (sdlog()) fprintf(stderr, "savedata: LOAD %s%s -> no data\n", game, save);
        return ERR_LOAD_NO_DATA;
    }
    /* An unfinished SAVE is settled before anything is read: no read ever sees half of one. */
    if (!savedata_recover(dir)) return ERR_LOAD_ACCESS_ERROR_UNMEASURED;
    if (fileName[0] && savedata_encrypted(dir, "LOAD")) return ERR_LOAD_DATA_BROKEN_UNMEASURED;
    MEM_W32(param + SDP_dataSize, 0);
    /* Blank fileName means success without reading data (PPSSPP LoadSaveData); the SFO and
     * icon blobs are still handed back. */
    if (fileName[0]) {
        uint32_t rd = 0;
        if (!dataBuf) {
            /* No buffer: the data file must exist, and none of it is read. */
            if (!host_file_stat(dir, fileName, NULL, NULL)) {
                if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> file not found\n", dir);
                return ERR_LOAD_NOT_FOUND;
            }
        } else {
            /* Issue #1387: one open measures and reads the file, so data that does not fit the
             * guest buffer is refused, not cut to the buffer size and reported as a success,
             * and a file replaced between a size probe and the read cannot come back short.
             * The code follows PPSSPP; the one a PSP returns here is UNMEASURED. */
            sr_guest_vram_write_prepare_bytes(dataBuf, cap);
            int got = host_read_exact(dir, fileName, (uint8_t *)SR_HOST(dataBuf), cap, &rd);
            sr_guest_vram_write_commit_bytes(dataBuf, got == 1 ? rd : 0u);
            if (got == 2) {
                fprintf(stderr, "savedata: LOAD %s/%s refused: %u bytes exceed the game's %" PRIu32
                        "-byte buffer\n", dir, fileName, (unsigned)rd, cap);
                return ERR_LOAD_DATA_BROKEN_UNMEASURED;
            }
            if (got != 1) {
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
    char rel[SR_CD_REL_MAX], dir[PATH_MAX];
    if (!save_rel(rel, sizeof(rel), game, save) || !save_dir(dir, sizeof(dir), game, save))
        return ERR_DELETE_NO_DATA;
    /* An unfinished SAVE is settled first, and a save this runtime cannot read is kept. */
    if (!savedata_recover(dir) || savedata_encrypted(dir, "DELETE"))
        return ERR_DELETE_ACCESS_ERROR_UNMEASURED;
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
    char rel[SR_CD_REL_MAX], dir[PATH_MAX], fileName[16];
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    if (!fileName[0] || !path_sanitize(fileName) || savedata_is_reserved_name(fileName))
        return ERR_RW_NO_DATA;
    if (!save_rel(rel, sizeof(rel), game, save) || !save_dir(dir, sizeof(dir), game, save))
        return ERR_RW_NO_DATA;
    /* ERASE removes one member, so the directory must be fully settled first: no record or
     * previous copy may still name the file it is about to delete. */
    if (!savedata_recover_settled(dir) || savedata_encrypted(dir, "ERASE"))
        return ERR_RW_NO_MEMSTICK_UNMEASURED;
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
/* Fields added by later parameter versions are absent from an older block, even
 * when the guest memory after it is mapped: an absent optional pointer reads as null. */
static uint32_t savedata_optional_pointer(uint32_t param, uint32_t size, uint32_t offset) {
    return offset <= size && size - offset >= 4u ? MEM_R32(param + offset) : 0u;
}

/* `count` entries of `bytes` each at `base`, all writable guest memory (checked arithmetic).
 * Nested output blocks are checked before the first byte of a result is written (#1386), and
 * only for what will be written: a count larger than the game's buffer holds is not a reason to
 * refuse while it asks for fewer. */
static int savedata_entries_writable(uint32_t base, uint32_t count, uint32_t bytes) {
    uint32_t span = 0;
    if (!count) return 1;
    return base && sr_size_mul_ok(count, bytes, &span) && sr_guest_span_writable(base, span);
}

#define SAVEDATA_IDLIST_BYTES   12u     /* SceUtilitySavedataIdListInfo: max, result, entries */
#define SAVEDATA_IDLIST_ENTRY   72u     /* SceUtilitySavedataIdListEntry */
#define SAVEDATA_FILELIST_BYTES 36u     /* SceUtilitySavedataFileListInfo */
#define SAVEDATA_FILELIST_ENTRY 80u     /* SceUtilitySavedataFileListEntry */

static uint32_t do_list(uint32_t param, uint32_t size, const char *game) {
    uint32_t idList = savedata_optional_pointer(param, size, SDP_idList);
    if (!idList) return 0;
    if (!sr_guest_span_readable(idList, SAVEDATA_IDLIST_BYTES) ||
        !sr_guest_span_writable(idList, SAVEDATA_IDLIST_BYTES)) {
        fprintf(stderr, "savedata: LIST refused: idList 0x%08" PRIx32 " is not a writable "
                "12-byte block\n", idList);
        return ERR_RW_BAD_PARAMS_UNMEASURED;
    }
    uint32_t maxCount = MEM_R32(idList + 0);
    uint32_t entries = MEM_R32(idList + 8);
    char root[PATH_MAX], path[PATH_MAX];
    snprintf(root, sizeof(root), "%s/PSP/SAVEDATA", ms_root());
#ifdef _WIN32
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)) ||
        !sr_vfs_dir_is_contained(root, canonical_root)) {
        MEM_W32(idList + 4, 0);
        return 0;
    }
#endif
    /* Enumerate first, then check the output span for exactly the rows that will be written,
     * then write: a refused request leaves the game's buffers untouched. */
    char **names = NULL;
    size_t name_count = 0;
    uint32_t found = 0;
    const char **rows = NULL;
    int64_t *times = NULL;
    size_t gl = strlen(game);
    if (gl && savedata_sorted_names(root, &names, &name_count) && name_count) {
        rows = (const char **)calloc(name_count, sizeof(*rows));
        times = (int64_t *)calloc(name_count, sizeof(*times));
        if (!rows || !times) {
            free(rows); free(times); free_names(names, name_count);
            return ERR_LOAD_ACCESS_ERROR_UNMEASURED;
        }
    }
    for (size_t i = 0; i < name_count; i++) {
        const char *name = names[i];
        if (strncmp(name, game, gl) != 0 || name[0] == '.') continue;
        /* A directory name is guest-influenced data at rest; the same
         * component rules apply before anything is opened or listed. */
        if (!path_sanitize(name)) continue;
        if (!path_join(path, sizeof(path), root, name)) continue;
#ifdef _WIN32
        if (!sr_vfs_dir_is_contained(path, canonical_root)) continue;
#endif
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        (void)savedata_recover(path);     /* settle an unfinished SAVE before its time is read */
        rows[found] = name;
        times[found] = savedata_save_time(path);
        found++;
    }
    uint32_t written = found < maxCount ? found : maxCount;
    if (written && !savedata_entries_writable(entries, written, SAVEDATA_IDLIST_ENTRY)) {
        fprintf(stderr, "savedata: LIST refused: %" PRIu32 " entries at 0x%08" PRIx32
                " are not writable\n", written, entries);
        free(rows); free(times); free_names(names, name_count);
        return ERR_RW_BAD_PARAMS_UNMEASURED;
    }
    for (uint32_t i = 0; i < written; i++) {
        uint32_t e = entries + i * SAVEDATA_IDLIST_ENTRY;
        MEM_W32(e + 0, 0x11FF);                       /* st_mode (directory) */
        put_psp_times(e + 4, times[i]);
        wr_fixed(e + 52, rows[i] + gl, 20);           /* saveName part only */
    }
    free(rows); free(times); free_names(names, name_count);
    MEM_W32(idList + 4, written);                      /* resultCount */
    if (sdlog()) fprintf(stderr, "savedata: LIST %s* -> %" PRIu32 " saves\n", game, written);
    return 0;
}

static int is_system_file(const char *n) {
    return !sd_stricmp(n, "PARAM.SFO") || !sd_stricmp(n, "ICON0.PNG") || !sd_stricmp(n, "ICON1.PMF") ||
           !sd_stricmp(n, "PIC1.PNG") || !sd_stricmp(n, "SND0.AT3");
}

/* FILES (12): list the files inside one save directory. */
static uint32_t do_files(uint32_t param, uint32_t size, const char *game, const char *save) {
    uint32_t fl = savedata_optional_pointer(param, size, SDP_fileList);
    if (!fl) return 0;
    if (!sr_guest_span_readable(fl, SAVEDATA_FILELIST_BYTES) ||
        !sr_guest_span_writable(fl, SAVEDATA_FILELIST_BYTES)) {
        fprintf(stderr, "savedata: FILES refused: fileList 0x%08" PRIx32 " is not a writable "
                "36-byte block\n", fl);
        return ERR_RW_BAD_PARAMS_UNMEASURED;
    }
    uint32_t maxSec = MEM_R32(fl + 0), maxNorm = MEM_R32(fl + 4), maxSys = MEM_R32(fl + 8);
    uint32_t pSec = MEM_R32(fl + 24), pNorm = MEM_R32(fl + 28), pSys = MEM_R32(fl + 32);
    char dir[PATH_MAX];
    char **names = NULL;
    size_t name_count = 0;
    int listed = save_dir(dir, sizeof(dir), game, save);
#ifdef _WIN32
    wchar_t canonical_root[MAX_PATH * 2];
    if (listed && (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)) ||
                   !sr_vfs_dir_is_contained(dir, canonical_root)))
        listed = 0;
#endif
    if (listed && dir_exists(dir) && !savedata_recover(dir)) {
        MEM_W32(fl + 12, 0); MEM_W32(fl + 16, 0); MEM_W32(fl + 20, 0);
        return ERR_LOAD_ACCESS_ERROR_UNMEASURED;
    }
    if (listed) listed = savedata_sorted_names(dir, &names, &name_count);
    if (!listed) {
        MEM_W32(fl + 12, 0); MEM_W32(fl + 16, 0); MEM_W32(fl + 20, 0);
        return ERR_LOAD_NO_DATA;                  /* no such save: FILES_NO_DATA semantics */
    }
    /* Each regular file once: a system file in the system list, any other file in the normal
     * list and again in the secure list (data files are also "secure"). */
    typedef struct { const char *name; uint64_t size; int64_t mtime; int sys; } FileRow;
    FileRow *rows = name_count ? (FileRow *)calloc(name_count, sizeof(*rows)) : NULL;
    if (name_count && !rows) {
        free_names(names, name_count);
        return ERR_LOAD_ACCESS_ERROR_UNMEASURED;
    }
    uint32_t nSec = 0, nNorm = 0, nSys = 0, found = 0;
    for (size_t i = 0; rows && i < name_count; i++) {
        const char *name = names[i];
        uint64_t fsize = 0;
        int64_t mtime = 0;
        if (name[0] == '.' || !path_sanitize(name)) continue;
        if (!host_file_stat(dir, name, &fsize, &mtime)) continue;
        rows[found].name = name;
        rows[found].size = fsize;
        rows[found].mtime = mtime;
        rows[found].sys = is_system_file(name);
        if (rows[found].sys) nSys++;
        else { nNorm++; nSec++; }
        found++;
    }
    uint32_t wSec = nSec < maxSec ? nSec : maxSec, wNorm = nNorm < maxNorm ? nNorm : maxNorm;
    uint32_t wSys = nSys < maxSys ? nSys : maxSys;
    if (!savedata_entries_writable(pSec, pSec ? wSec : 0u, SAVEDATA_FILELIST_ENTRY) ||
        !savedata_entries_writable(pNorm, pNorm ? wNorm : 0u, SAVEDATA_FILELIST_ENTRY) ||
        !savedata_entries_writable(pSys, pSys ? wSys : 0u, SAVEDATA_FILELIST_ENTRY)) {
        fprintf(stderr, "savedata: FILES refused: an entry array is not writable\n");
        free(rows); free_names(names, name_count);
        return ERR_RW_BAD_PARAMS_UNMEASURED;
    }
    uint32_t iSec = 0, iNorm = 0, iSys = 0;
    for (uint32_t i = 0; i < found; i++) {
        for (int pass = 0; pass < (rows[i].sys ? 1 : 2); pass++) {
            uint32_t *at = rows[i].sys ? &iSys : pass ? &iSec : &iNorm;
            uint32_t base = rows[i].sys ? pSys : pass ? pSec : pNorm;
            uint32_t cap = rows[i].sys ? wSys : pass ? wSec : wNorm;
            if (base && *at < cap) {
                uint32_t e = base + *at * SAVEDATA_FILELIST_ENTRY;
                MEM_W32(e + 0, 0x21FF);                    /* st_mode (file) */
                MEM_W32(e + 4, 0);
                MEM_W32(e + 8, (uint32_t)rows[i].size);
                MEM_W32(e + 12, (uint32_t)(rows[i].size >> 32));
                put_psp_times(e + 16, rows[i].mtime);
                wr_fixed(e + 64, rows[i].name, 16);
            }
            (*at)++;
        }
    }
    free(rows); free_names(names, name_count);
    MEM_W32(fl + 12, wSec); MEM_W32(fl + 16, wNorm); MEM_W32(fl + 20, wSys);
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

#define SAVEDATA_MSFREE_BYTES   20u     /* SceUtilitySavedataMsFreeInfo */
#define SAVEDATA_MSDATA_BYTES   64u     /* SceUtilitySavedataMsDataInfo: names + used info */
#define SAVEDATA_USEDDATA_BYTES 28u     /* SceUtilitySavedataUsedDataInfo */

static uint32_t do_sizes(uint32_t param, uint32_t size, const char *game) {
    uint32_t msFree = savedata_optional_pointer(param, size, SDP_msFree);
    uint32_t msData = savedata_optional_pointer(param, size, SDP_msData);
    uint32_t ud = savedata_optional_pointer(param, size, SDP_usedData);
    /* Every nested output block is checked before the first is written (#1386). */
    if ((msFree && !sr_guest_span_writable(msFree, SAVEDATA_MSFREE_BYTES)) ||
        (msData && (!sr_guest_span_readable(msData, SAVEDATA_MSDATA_BYTES) ||
                    !sr_guest_span_writable(msData, SAVEDATA_MSDATA_BYTES))) ||
        (ud && !sr_guest_span_writable(ud, SAVEDATA_USEDDATA_BYTES))) {
        fprintf(stderr, "savedata: SIZES refused: an output block (msFree 0x%08" PRIx32
                ", msData 0x%08" PRIx32 ", usedData 0x%08" PRIx32 ") is not writable\n",
                msFree, msData, ud);
        return ERR_SIZES_PARAM_UNMEASURED;
    }
    if (msFree) {
        SavedataFree fr;
        if (!savedata_free_space(&fr)) return ERR_SIZES_ACCESS_ERROR_UNMEASURED;
        MEM_W32(msFree + 0, CLUSTER);
        MEM_W32(msFree + 4, fr.sectors);
        MEM_W32(msFree + 8, fr.kb);
        wr_fixed(msFree + 12, fr.text, 8);
    }
    int sizes_no_data = 0;
    if (msData) {
        /* info block at +36: used clusters/KB of the named save (0 if absent) */
        char game2[16], save2[24], dir[PATH_MAX];
        rd_cstr(msData + 0, game2, 14);
        rd_cstr(msData + 16, save2, 21);
        if (save_dir(dir, sizeof(dir), game2[0] ? game2 : game, save2) && dir_exists(dir)) {
            if (!savedata_recover(dir)) return ERR_SIZES_ACCESS_ERROR_UNMEASURED;
            uint64_t used = 0;
            char **names = NULL;
            size_t name_count = 0;
            if (savedata_sorted_names(dir, &names, &name_count)) {
                for (size_t i = 0; i < name_count; i++) {
                    uint64_t fsize = 0;
                    if (!path_sanitize(names[i]) || !host_file_stat(dir, names[i], &fsize, NULL))
                        continue;
                    used += (fsize + CLUSTER - 1) / CLUSTER * CLUSTER;
                }
                free_names(names, name_count);
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
    uint64_t size = 0;
    if (!dir[0] || !name[0] || savedata_is_reserved_name(name) ||
        !host_file_stat(dir, name, &size, NULL))
        return 0;
    return size;
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
static uint32_t do_getsize(uint32_t param, uint32_t size, const char *game, const char *save) {
    uint32_t si = savedata_optional_pointer(param, size, SDP_sizeInfo);
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
    if (save_dir(dir, sizeof(dir), game, save) && !savedata_recover(dir))
        return ERR_RW_NO_MEMSTICK_UNMEASURED;
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

/* ---- list selection (#1387) ----------------------------------------------------------------
 * SceUtilitySavedataParam focus (offset 0x5c8, after newData): which row of a list-mode
 * dialog's save list holds the cursor when the dialog opens (PSPSDK psputility_savedata.h;
 * PPSSPP SceUtilitySavedataFocus and PSPSaveDialog::Init, revision c9e2afb6, as the behaviour
 * reference; UNMEASURED on a PSP). A 0x5c8-byte block ends before it, so there it reads as
 * NAME. */
#define SDP_focus 0x5c8
enum { FOCUS_NAME = 0, FOCUS_FIRSTLIST = 1, FOCUS_LASTLIST = 2, FOCUS_LATEST = 3,
       FOCUS_OLDEST = 4, FOCUS_FIRSTDATA = 5, FOCUS_LASTDATA = 6, FOCUS_FIRSTEMPTY = 7,
       FOCUS_LASTEMPTY = 8 };
#define SAVEDATA_LIST_ENTRY 20u          /* saveNameList: char[20] entries, "" ends it */

typedef struct {
    char name[24];
    int exists;
    int64_t time;
} SaveListRow;

static int save_list_has(const SaveListRow *rows, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(rows[i].name, name)) return 1;
    return 0;
}

/* One row: whether this game's save of that name exists, and its time ("save times"). An
 * unfinished SAVE in it is settled first, so a half-written save is never judged. */
static void save_list_add(SaveListRow *row, const char *game, const char *name) {
    char dir[PATH_MAX];
    snprintf(row->name, sizeof(row->name), "%s", name);
    row->exists = save_dir(dir, sizeof(dir), game, name) && dir_exists(dir);
    if (row->exists) (void)savedata_recover(dir);
    row->time = row->exists ? savedata_save_time(dir) : 0;
}

/* The row the cursor starts on (PPSSPP PSPSaveDialog::Init). LATEST and OLDEST compare the
 * save times of rows that hold a save, earliest row on a tie; every rule falls back to row 0
 * when nothing matches, and so does an unknown focus, as there. */
static size_t save_list_focus(const SaveListRow *rows, size_t count, uint32_t focus,
                              const char *save_name) {
    size_t pick = 0;
    int found = 0;
    switch (focus) {
        case FOCUS_NAME:
            for (size_t i = 0; i < count && !found; i++)
                if (!strcmp(rows[i].name, save_name)) { pick = i; found = 1; }
            break;
        case FOCUS_FIRSTLIST:
            break;
        case FOCUS_LASTLIST:
            pick = count - 1u;
            break;
        case FOCUS_LATEST:
        case FOCUS_OLDEST:
            for (size_t i = 0; i < count; i++) {
                if (!rows[i].exists) continue;
                int better = focus == FOCUS_LATEST ? rows[i].time > rows[pick].time
                                                   : rows[i].time < rows[pick].time;
                if (!found || better) { pick = i; found = 1; }
            }
            break;
        case FOCUS_FIRSTDATA:
        case FOCUS_FIRSTEMPTY:
            for (size_t i = 0; i < count && !found; i++)
                if (rows[i].exists == (focus == FOCUS_FIRSTDATA)) { pick = i; found = 1; }
            break;
        case FOCUS_LASTDATA:
        case FOCUS_LASTEMPTY:
            for (size_t i = count; i > 0 && !found; i--)
                if (rows[i - 1u].exists == (focus == FOCUS_LASTDATA)) { pick = i - 1u; found = 1; }
            break;
        default:
            break;
    }
    return pick;
}

/* What resolve_save decided. */
typedef struct {
    int cancel;             /* the dialog closes with result CANCEL; nothing is read or written */
    int keep_previous;      /* an automatic LISTSAVE onto an existing save keeps what it replaces */
} SaveChoice;

/* Resolve the effective saveName (PPSSPP SavedataParam::SetPspParam and GetSaveName).
 *
 * A saveName of "<>" is the empty name. Only the list modes read saveNameList: the dialog lists
 * one row per entry in list order, LISTSAVE keeping the entries that have no save yet and
 * LISTLOAD/LISTDELETE dropping them, and a "<>" entry stands for the first save of this game,
 * in sorted directory order, that the list does not already hold. The cursor starts on the row
 * the focus field names.
 *
 * There is no savedata list UI, so a person cannot move the cursor. SR_SAVEDATA_LIST_SELECT
 * picks for them: a saveName chooses that row, CANCEL (or a name that is not a row) cancels.
 * Without it:
 *  - LISTLOAD loads the focus row, as a person pressing X at once would;
 *  - LISTSAVE saves to the focus row, and when that row already holds a save, the version it
 *    replaces is kept (SaveChoice.keep_previous), because nobody chose to overwrite it;
 *  - LISTDELETE cancels: no save is deleted that nobody chose.
 * Every selection is logged. A list mode whose list is absent or empty, or a LISTSAVE list that
 * yields no row, uses saveName. Returns 0 or a result code: LISTLOAD/LISTDELETE with no listed
 * save (the dialog's "There is no data"), or an unreadable list. */
static uint32_t resolve_save(uint32_t param, uint32_t size, uint32_t mode, const char *game,
                             char *save, int cap, SaveChoice *choice) {
    memset(choice, 0, sizeof(*choice));
    rd_cstr(param + SDP_saveName, save, cap);
    const int isList = (mode == SD_LISTLOAD || mode == SD_LISTSAVE || mode == SD_LISTDELETE);
    if (!strcmp(save, "<>")) {
        save[0] = 0;
        if (!isList)
            fprintf(stderr, "savedata: mode %" PRIu32 ": saveName \"<>\" is the empty name: the save "
                    "named by the game name alone\n", mode);
    }
    uint32_t list = MEM_R32(param + SDP_saveNameList);
    if (!isList || !list) return 0;

    SaveListRow rows[SAVEDATA_MAX_FILE_ENTRIES];
    size_t count = 0, entries = 0;
    char **names = NULL;
    size_t name_count = 0;
    int names_read = 0;
    char ent[24], root[PATH_MAX], path[PATH_MAX];
    snprintf(root, sizeof(root), "%s/PSP/SAVEDATA", ms_root());
    for (; entries < SAVEDATA_MAX_FILE_ENTRIES; entries++) {
        uint32_t at = list + (uint32_t)entries * SAVEDATA_LIST_ENTRY;
        if (!sr_guest_span_readable(at, SAVEDATA_LIST_ENTRY)) {
            free_names(names, name_count);
            fprintf(stderr, "savedata: list mode %" PRIu32 " refused: saveNameList entry %zu at "
                    "0x%08" PRIx32 " is not readable\n", mode, entries, at);
            return mode == SD_LISTSAVE ? ERR_SAVE_PARAM_UNMEASURED
                 : mode == SD_LISTDELETE ? 0x80110348u /* DELETE_PARAM */ : 0x80110308u /* LOAD_PARAM */;
        }
        rd_cstr(at, ent, 21);
        if (!ent[0]) break;
        if (!strcmp(ent, "<>")) {
            if (!names_read) names_read = savedata_sorted_names(root, &names, &name_count) ? 1 : -1;
            size_t gl = strlen(game);
            for (size_t i = 0; names_read > 0 && gl && i < name_count; i++) {
                const char *name = names[i];
                struct stat st;
                if (strncmp(name, game, gl) != 0 || !path_sanitize(name)) continue;
                if (!path_join(path, sizeof(path), root, name)) continue;
                if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (strlen(name + gl) > 20u || save_list_has(rows, count, name + gl)) continue;
                save_list_add(&rows[count], game, name + gl);
                if (rows[count].exists) count++;
                break;
            }
            continue;
        }
        save_list_add(&rows[count], game, ent);
        if (rows[count].exists || mode == SD_LISTSAVE) count++;
    }
    free_names(names, name_count);
    if (!entries) return 0;

    static const char *const mode_names[] = { "LISTLOAD", "LISTSAVE", "LISTDELETE" };
    const char *mode_name = mode_names[mode - SD_LISTLOAD];
    const uint32_t focus = SDP_focus + 4u <= size ? MEM_R32(param + SDP_focus) : FOCUS_NAME;
    if (!count && mode == SD_LISTSAVE) {
        fprintf(stderr, "savedata: LISTSAVE list of %zu names has no row; saving to saveName\n", entries);
        return 0;
    }
    if (!count) {
        fprintf(stderr, "savedata: %s list of %zu names: no save exists\n", mode_name, entries);
        return mode == SD_LISTDELETE ? ERR_DELETE_NO_DATA : ERR_LOAD_NO_DATA;
    }
    size_t pick = save_list_focus(rows, count, focus, save);
    const char *select = getenv("SR_SAVEDATA_LIST_SELECT");
    if (select && *select) {
        size_t chosen = count;
        for (size_t i = 0; strcmp(select, "CANCEL") != 0 && i < count; i++)
            if (!strcmp(rows[i].name, select)) chosen = i;
        if (chosen == count) {
            fprintf(stderr, "savedata: %s cancelled by SR_SAVEDATA_LIST_SELECT=%s (%zu rows)\n",
                    mode_name, select, count);
            choice->cancel = 1;
            return 0;
        }
        pick = chosen;
    } else if (mode == SD_LISTDELETE) {
        fprintf(stderr, "savedata: LISTDELETE of %zu rows cancelled: there is no list UI and no save "
                "is deleted that nobody chose (SR_SAVEDATA_LIST_SELECT=<saveName> chooses one)\n", count);
        choice->cancel = 1;
        return 0;
    } else if (mode == SD_LISTSAVE && rows[pick].exists) {
        choice->keep_previous = 1;
    }
    snprintf(save, (size_t)cap, "%s", rows[pick].name);
    fprintf(stderr, "savedata: %s list of %zu rows, focus %" PRIu32 ": %s row %zu '%s'%s\n",
            mode_name, count, focus, select && *select ? "SR_SAVEDATA_LIST_SELECT chose" : "selects",
            pick, save, choice->keep_previous ? "; the save it replaces is kept" : "");
    return 0;
}

/* The access-error result of each mode family, for a busy lock or a directory recovery left
 * failed closed. UNMEASURED (PPSSPP names); the MAKEDATA/READDATA/WRITEDATA family has no
 * access error and reports the Memory Stick as unavailable. */
static uint32_t savedata_access_error(uint32_t mode) {
    switch (mode) {
        case SD_AUTOLOAD: case SD_LOAD: case SD_LISTLOAD: case SD_FILES:
            return ERR_LOAD_ACCESS_ERROR_UNMEASURED;
        case SD_AUTOSAVE: case SD_SAVE: case SD_LISTSAVE:
            return ERR_SAVE_ACCESS_ERROR_UNMEASURED;
        case SD_LISTDELETE: case SD_DELETE: case SD_AUTODELETE:
            return ERR_DELETE_ACCESS_ERROR_UNMEASURED;
        case SD_SIZES:
            return ERR_SIZES_ACCESS_ERROR_UNMEASURED;
        default:
            return ERR_RW_NO_MEMSTICK_UNMEASURED;
    }
}

static uint32_t savedata_dispatch(uint32_t param, uint32_t size, uint32_t mode, const char *game,
                                  const char *save, const SaveChoice *choice) {
    switch (mode) {
        case SD_AUTOSAVE: case SD_SAVE: case SD_LISTSAVE:
        case SD_MAKEDATA: case SD_MAKEDATASECURE:
        case SD_WRITEDATA: case SD_WRITEDATASECURE: {
            uint32_t r = do_save(param, game, save, choice->keep_previous);
            /* The MAKEDATA/WRITEDATA family reports in its own RW codes (PPSSPP): a full volume
             * as RW_MEMSTICK_FULL, a refused name as RW_BAD_PARAMS, an access failure as an
             * unavailable Memory Stick. Only a full volume is ever reported as full. */
            if (mode != SD_AUTOSAVE && mode != SD_SAVE && mode != SD_LISTSAVE) {
                if (r == ERR_SAVE_NOSPACE_UNMEASURED) r = ERR_RW_MS_FULL;
                else if (r == ERR_SAVE_PARAM_UNMEASURED) r = ERR_RW_BAD_PARAMS_UNMEASURED;
                else if (r == ERR_SAVE_ACCESS_ERROR_UNMEASURED) r = ERR_RW_NO_MEMSTICK_UNMEASURED;
            }
            return r;
        }
        case SD_AUTOLOAD: case SD_LOAD: case SD_LISTLOAD:
            return do_load(param, game, save);
        case SD_READDATA: case SD_READDATASECURE: {
            uint32_t r = do_load(param, game, save);             /* PPSSPP error remap */
            if (r == ERR_LOAD_NO_DATA) r = ERR_RW_NO_DATA;
            if (r == ERR_LOAD_NOT_FOUND) r = ERR_RW_NOT_FOUND;
            if (r == ERR_LOAD_ACCESS_ERROR_UNMEASURED) r = ERR_RW_NO_MEMSTICK_UNMEASURED;
            return r;
        }
        case SD_LISTDELETE: case SD_DELETE: case SD_AUTODELETE:
            return do_delete(game, save);
        case SD_DELETEDATA: {
            uint32_t r = do_delete(game, save);
            if (r == ERR_DELETE_ACCESS_ERROR_UNMEASURED) r = ERR_RW_NO_MEMSTICK_UNMEASURED;
            return r == ERR_DELETE_NO_DATA ? ERR_RW_NO_DATA : r;
        }
        case SD_ERASE: case SD_ERASESECURE:
            return do_erase(param, game, save);
        case SD_LIST:
            return do_list(param, size, game);
        case SD_FILES:
            return do_files(param, size, game, save);
        case SD_SIZES:
            return do_sizes(param, size, game);
        case SD_GETSIZE:
            return do_getsize(param, size, game, save);
        default:
            /* LISTALLDELETE (7) and any mode past 22 are not implemented: a named refusal, never a
             * success that did nothing. The code is PPSSPP's SAVEDATA_ERROR_TYPE, UNMEASURED. */
            fprintf(stderr, "savedata: mode %" PRIu32 " is not implemented; refused with 0x%08x\n",
                    mode, (unsigned)ERR_TYPE_UNMEASURED);
            return ERR_TYPE_UNMEASURED;
    }
}

/* size is the parameter block's own size word, one of the three versions InitStart accepts
 * (0x5c8, 0x5dc, 0x600; pspautotests e2d098fc): nothing past it is read or written. The whole
 * operation runs under the root lock (savedata_lock). */
uint32_t sr_savedata_execute_sized(uint32_t param, uint32_t size) {
    if (!param || (size != 0x5c8u && size != 0x5dcu && size != 0x600u) ||
        !sr_guest_span_readable(param, size) || !sr_guest_span_writable(param, size)) return 0x80110381u;
    uint32_t mode = MEM_R32(param + SDP_mode);
    char game[16], save[24];
    SaveChoice choice;
    rd_cstr(param + SDP_gameName, game, 14);
    const int writes = mode == SD_AUTOSAVE || mode == SD_SAVE || mode == SD_LISTSAVE ||
                       mode == SD_MAKEDATA || mode == SD_MAKEDATASECURE ||
                       mode == SD_WRITEDATA || mode == SD_WRITEDATASECURE;
    const int lock = savedata_lock(writes);
    if (lock == SD_LOCK_BUSY) return savedata_access_error(mode);
    uint32_t r = resolve_save(param, size, mode, game, save, 21, &choice);
    if (sdlog()) fprintf(stderr, "savedata: mode=%" PRIu32 " game='%s' save='%s'\n", mode, game, save);
    if (!r) r = choice.cancel ? SAVEDATA_DIALOG_RESULT_CANCEL
                              : savedata_dispatch(param, size, mode, game, save, &choice);
    savedata_unlock(lock);
    return r;
}

/* Internal callers pass a full-sized (0x600-byte) block. */
uint32_t sr_savedata_execute(uint32_t param) {
    return sr_savedata_execute_sized(param, 0x600u);
}
