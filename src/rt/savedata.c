// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

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
 * GETSIZE report a roomy fake memory stick; the DELETE family removes a save directory.
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

/* SceUtilitySavedataParam: bind result (offset 0x34, after mode). PPSSPP sets 1021 on every
 * successful load -- "PSP always responds this and this unlocks some games". */
#define SDP_bind 0x34

#define CLUSTER 0x8000u          /* 32 KB "memory stick" cluster */
#define FREE_CLUSTERS 0x4000u    /* pretend 512 MB free */

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
    const char *r = getenv("SR_MEMSTICK");
    return r && *r ? r : "memstick";
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

/* memstick/PSP/SAVEDATA/<gameName><saveName> */
static void save_dir(char *out, int cap, const char *game, const char *save) {
    if (!path_sanitize(game) || !path_sanitize(save)) {
        snprintf(out, cap, "%s/PSP/SAVEDATA/INVALID", ms_root());
        return;
    }
    snprintf(out, cap, "%s/PSP/SAVEDATA/%s%s", ms_root(), game, save);
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
    if (!path_sanitize(game) || !path_sanitize(save)) return 0;
    int n = snprintf(leaf, sizeof(leaf), "%s%s", game, save);
    if (n <= 0 || (size_t)n >= sizeof(leaf)) return 0;
    if (!sr_cd_component_is_generic(leaf, (size_t)n) ||
        !sr_vfs_is_safe_component(leaf, (size_t)n)) return 0;
    n = snprintf(out, cap, "PSP/SAVEDATA/%s", leaf);
    return n > 0 && (size_t)n < cap;
}

static int sdlog(void) { static int v = -1; if (v < 0) v = getenv("SR_DLGLOG") ? 1 : 0; return v; }

static int host_write_file(const char *dir, const char *name, const uint8_t *data, uint32_t n) {
    if (!path_sanitize(name)) { errno = EINVAL; return 0; }
#ifdef _WIN32
    char path[PATH_MAX];
    if (!path_join(path, sizeof(path), dir, name)) return 0;
    /* OPEN -> HANDLE -> FINAL PATH VERIFY -> OPERATION. The write lands through
     * the very handle whose final path was verified, so a swap after the check
     * cannot redirect the bytes. OPEN_ALWAYS + SetEndOfFile reproduces the old
     * "wb" truncate-or-create semantics. */
    wchar_t canonical_root[MAX_PATH * 2];
    if (!ms_canonical_root(canonical_root, sizeof(canonical_root)/sizeof(wchar_t)))
        return 0;
    HANDLE h;
    if (!sr_vfs_open_contained_utf8(path, GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL,
                                    OPEN_ALWAYS, canonical_root, &h))
        return 0;
    int ok = SetFilePointer(h, 0, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
             SetEndOfFile(h);
    DWORD written = 0;
    if (ok && n > 0 && data) {
        ok = WriteFile(h, data, (DWORD)n, &written, NULL) && written == (DWORD)n;
    }
    CloseHandle(h);
    return ok ? 1 : 0;
#else
#if defined(SR_CD_BACKEND_POSIX_AT)
    char rel[SR_CD_REL_MAX];
    if (!savedata_root_relative(dir, rel, sizeof(rel))) return 0;
    sr_cd_root root;
    if (sr_cd_root_open(ms_root(), &root) != SR_CD_OK) return 0;
    FILE *f = NULL;
    sr_cd_status st = sr_cd_open_file(&root, rel, name, SR_CD_FILE_WRITE_TRUNCATE, &f);
    sr_cd_root_close(&root);
    if (st != SR_CD_OK || !f) return 0;
    int ok = !n || fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    return ok;
#else
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

/* Storage boundary: PSP serialization/crypto can be inserted by replacing these operations;
 * dialog and guest-memory code never depends on the host filesystem representation. */
typedef struct SavedataStorageOps {
    int (*prepare)(const char *dir);
    int (*write)(const char *dir, const char *name, const uint8_t *data, uint32_t size);
    int (*read)(const char *dir, const char *name, uint8_t *data, uint32_t cap, uint32_t *size);
} SavedataStorageOps;

static const SavedataStorageOps s_storage = { mkdirs, host_write_file, host_read_file };

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

static void sfo_write(const char *dir, const char *title, const char *saveTitle,
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
        if (e[i].len > e[i].maxlen) return;
        keyOff[i] = keySize; keySize += (uint32_t)strlen(e[i].key) + 1;
        dataOff[i] = dataSize; dataSize += (e[i].maxlen + 3) & ~3u;
    }
    uint32_t keyStart = 20 + 16u * (uint32_t)n;
    uint32_t dataStart = (keyStart + keySize + 3) & ~3u;
    uint32_t totalAlloc = dataStart + dataSize;

    uint8_t *buf = (uint8_t *)calloc(1, totalAlloc);
    if (!buf) return;
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
    s_storage.write(dir, "PARAM.SFO", buf, totalAlloc);
    free(buf);
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
    return sr_guest_span_readable(buf, sz);
}

static int validate_filedata_writable(uint32_t fd) {
    if (!fd) return 1;
    uint32_t buf = MEM_R32(fd + 0), cap = MEM_R32(fd + 4);
    return sr_guest_span_writable(buf, cap);
}

static void write_filedata(const char *dir, const char *name, uint32_t fd) {
    uint32_t buf = MEM_R32(fd + 0), sz = MEM_R32(fd + 8);
    if (!buf || !sz) return;
    if (!sr_guest_span_readable(buf, sz)) return;
    s_storage.write(dir, name, (const uint8_t *)SR_HOST(buf), sz);
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
        if (!dataBuf || dataSize >= 0x04000000u || !sr_guest_span_readable(dataBuf, dataSize))
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
    if (fileName[0] && dataBuf && dataSize && dataSize < 0x04000000) {
        if (!s_storage.write(dir, fileName, (const uint8_t *)SR_HOST(dataBuf), dataSize))
            return 0x80110381u;   /* SAVE_NO_MS: couldn't write */
    }
    rd_cstr(param + SDP_sfoTitle, title, sizeof(title));
    rd_cstr(param + SDP_sfoSaveTitle, saveTitle, sizeof(saveTitle));
    rd_cstr(param + SDP_sfoDetail, detail, sizeof(detail));
    snprintf(saveDir, sizeof(saveDir), "%s%s", game, save);
    sfo_write(dir, title, saveTitle, detail, saveDir, MEM_R8(param + SDP_sfoParental), fileName);
    write_filedata(dir, "ICON0.PNG", param + SDP_icon0);
    write_filedata(dir, "ICON1.PMF", param + SDP_icon1);
    write_filedata(dir, "PIC1.PNG", param + SDP_pic1);
    write_filedata(dir, "SND0.AT3", param + SDP_snd0);
    if (sdlog()) fprintf(stderr, "savedata: SAVE %s\\%s (%u bytes)\n", dir, fileName, dataSize);
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
    if (s_storage.read(dir, name, (uint8_t *)SR_HOST(buf), cap, &rd)) MEM_W32(fd + 8, rd);
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
        size_t to_read = 0;
        if (n > 0) {
            uint64_t sz = (uint64_t)n;
            if (sz > (uint64_t)cap) sz = (uint64_t)cap;
            to_read = (size_t)sz;
        }
        uint32_t rd = 0;
        if (dataBuf && to_read > 0) {
            if (!s_storage.read(dir, fileName, (uint8_t *)SR_HOST(dataBuf), (uint32_t)to_read, &rd)) {
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
            fprintf(stderr, "savedata: LOAD %s (%u bytes) -> buf=0x%08x cap=%u first=%02x%02x%02x%02x\n",
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
    if (sdlog()) fprintf(stderr, "savedata: LIST %s* -> %u saves\n", game, count);
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
        if (de->d_name[0] == '.') continue;
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

static uint32_t do_sizes(uint32_t param, const char *game) {
    uint32_t msFree = MEM_R32(param + SDP_msFree);
    if (msFree) {
        MEM_W32(msFree + 0, CLUSTER);
        MEM_W32(msFree + 4, FREE_CLUSTERS);
        MEM_W32(msFree + 8, FREE_CLUSTERS * (CLUSTER / 0x400));
        wr_fixed(msFree + 12, "512 MB", 8);
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

static int getsize_add_entry(uint64_t *needed, uint32_t entries, uint32_t index) {
    uint32_t offset, entry;
    if (!needed || !sr_size_mul_ok(index, SAVEDATA_SIZE_ENTRY_BYTES, &offset) ||
        !sr_size_add_ok(entries, offset, &entry)) return 0;
    uint64_t size = MEM_R32(entry) | ((uint64_t)MEM_R32(entry + 4) << 32);
    if (size > UINT64_MAX - (uint64_t)(CLUSTER - 1u)) return 0;
    uint64_t rounded = ((size + (uint64_t)(CLUSTER - 1u)) / CLUSTER) * CLUSTER;
    if (rounded > UINT64_MAX - *needed) return 0;
    *needed += rounded;
    return 1;
}

/* GETSIZE (22): free/needed space for the sizeInfo block. */
static uint32_t do_getsize(uint32_t param) {
    uint32_t si = MEM_R32(param + SDP_sizeInfo);
    if (!si) return 0;
    if (!sr_guest_span_readable(si, SAVEDATA_SIZE_INFO_BYTES) ||
        !sr_guest_span_writable(si, SAVEDATA_SIZE_INFO_BYTES)) return 0x80110381u;
    uint32_t nSec = MEM_R32(si + 0), nNorm = MEM_R32(si + 4);
    uint32_t pSec = MEM_R32(si + 8), pNorm = MEM_R32(si + 12);
    uint32_t secSpan = 0, normSpan = 0;
    if (!validate_getsize_entries(nSec, pSec, &secSpan) ||
        !validate_getsize_entries(nNorm, pNorm, &normSpan)) return 0x80110381u;
    uint64_t needed = CLUSTER + CLUSTER;                    /* dir record + SFO */
    (void)secSpan;
    (void)normSpan;
    for (uint32_t i = 0; i < nSec; i++) {
        if (!getsize_add_entry(&needed, pSec, i)) return 0x80110381u;
    }
    for (uint32_t i = 0; i < nNorm; i++) {
        if (!getsize_add_entry(&needed, pNorm, i)) return 0x80110381u;
    }
    MEM_W32(si + 16, CLUSTER);                              /* sectorSize */
    MEM_W32(si + 20, FREE_CLUSTERS);                        /* freeSectors */
    MEM_W32(si + 24, FREE_CLUSTERS * (CLUSTER / 0x400));    /* freeKB */
    wr_fixed(si + 28, "512 MB", 8);
    MEM_W32(si + 36, (uint32_t)(needed / 0x400));           /* neededKB */
    wr_fixed(si + 40, "", 8);
    MEM_W32(si + 48, (uint32_t)(needed / 0x400));           /* overwriteKB */
    wr_fixed(si + 52, "", 8);
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
    if (sdlog()) fprintf(stderr, "savedata: mode=%u game='%s' save='%s'\n", mode, game, save);
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
            return do_getsize(param);
        default:
            if (sdlog()) fprintf(stderr, "savedata: UNHANDLED mode=%u\n", mode);
            return 0;
    }
}
