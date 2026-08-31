// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Executable evidence for the contained-delete seam (vfs_contained.h).
 *
 * Everything here runs against synthetic directories created under the
 * process's own temporary area: no retail input, no game input, and no path
 * that a real user's files could ever occupy. Each case builds its own fixture,
 * asserts, and tears it down.
 *
 * The hostile cases are the point. The pre-fix POSIX savedata deletion
 * re-resolved a guest-influenced pathname on every step -- stat(path) then
 * unlink(path), opendir(dir) then rmdir(dir) -- so an actor able to replace an
 * intermediate save-directory component redirected the deletion outside the
 * memstick root. Case H1 performs that legacy sequence for real and shows the
 * outside victim being destroyed; case H2 rebuilds the identical fixture and
 * shows the seam refusing it with the victim intact. That contrast, not a
 * comment, is what pins the fix.
 */

#include "vfs_contained.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define t_mkdir(p) _mkdir(p)
#define t_rmdir(p) _rmdir(p)
#define t_unlink(p) _unlink(p)
#else
/* The fixture teardown enumerates directories itself. The seam's own headers
 * pull dirent.h in only when a real backend is selected, so the
 * force-unsupported build needs it named here. */
#include <dirent.h>
#include <unistd.h>
#define t_mkdir(p) mkdir((p), 0700)
#define t_rmdir(p) rmdir(p)
#define t_unlink(p) unlink(p)
#endif

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL L%d: ", __LINE__);                  \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            g_failed = 1;                                             \
        }                                                             \
    } while (0)

#define CHECK_ST(actual, expected)                                    \
    do {                                                              \
        sr_cd_status a_ = (actual), e_ = (expected);                  \
        CHECK(a_ == e_, "status %s, expected %s",                     \
              sr_cd_status_name(a_), sr_cd_status_name(e_));          \
    } while (0)

/* ---- fixture helpers ---------------------------------------------------- */

static char g_tmp[1024];

/* Fixture path builder. A runtime format keeps the compiler's truncation
 * analysis out of every call site while still refusing to build a truncated
 * fixture path: a fixture that silently pointed somewhere else would make every
 * assertion below meaningless. */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void fp(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "fixture path does not fit (%d bytes)\n", n);
        exit(2);
    }
}

static int exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path, &st) == 0;
#endif
}

static int is_dir(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

/* Build a temporary sandbox root unique to this process. */
static int make_sandbox(void) {
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(base), base);
    if (n == 0 || n >= sizeof(base)) return 0;
    fp(g_tmp, sizeof(g_tmp), "%ssr_cd_%lu", base, (unsigned long)GetCurrentProcessId());
#else
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    fp(g_tmp, sizeof(g_tmp), "%s/sr_cd_%ld", base, (long)getpid());
#endif
    if (t_mkdir(g_tmp) != 0 && !is_dir(g_tmp)) return 0;
    return 1;
}

/* Depth-first teardown of the sandbox, following nothing. */
static void nuke(const char *path) {
    if (!exists(path)) return;
    if (is_dir(path)) {
#ifdef _WIN32
        char pat[1200];
        WIN32_FIND_DATAA fd;
        fp(pat, sizeof(pat), "%s\\*", path);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                char child[1200];
                fp(child, sizeof(child), "%s\\%s", path, fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryA(child);
                    else DeleteFileA(child);
                } else {
                    nuke(child);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char child[1200];
                fp(child, sizeof(child), "%s/%s", path, de->d_name);
                struct stat st;
                if (lstat(child, &st) == 0 && S_ISLNK(st.st_mode)) unlink(child);
                else nuke(child);
            }
            closedir(d);
        }
#endif
        t_rmdir(path);
    } else {
        t_unlink(path);
    }
}

/* Create <root>/PSP/SAVEDATA/<save>/ holding `files`, and return the root in
 * `root_out`. Every fixture is a fresh subdirectory of the sandbox. */
static int build_save(const char *case_name, const char *save, const char *const *files,
                      size_t nfiles, char *root_out, size_t cap) {
    char psp[1100], sd[1100], dir[1100], f[1200];
    fp(root_out, cap, "%s/%s", g_tmp, case_name);
    nuke(root_out);
    if (t_mkdir(root_out) != 0) return 0;
    fp(psp, sizeof(psp), "%s/PSP", root_out);
    if (t_mkdir(psp) != 0) return 0;
    fp(sd, sizeof(sd), "%s/SAVEDATA", psp);
    if (t_mkdir(sd) != 0) return 0;
    if (!save) return 1;
    fp(dir, sizeof(dir), "%s/%s", sd, save);
    if (t_mkdir(dir) != 0) return 0;
    for (size_t i = 0; i < nfiles; i++) {
        fp(f, sizeof(f), "%s/%s", dir, files[i]);
        if (!write_file(f, "payload")) return 0;
    }
    return 1;
}

/* ---- cases -------------------------------------------------------------- */

static const char *const SAVE_FILES[] = { "DATA.BIN", "PARAM.SFO", "ICON0.PNG" };

/* The hostile cases only exist for a build that HAS a backend. The
 * force-unsupported build compiles the fail-closed contract instead. */
#ifndef SR_CD_FORCE_UNSUPPORTED_BACKEND

/* Make a directory symlink / junction. Returns 0 when the host will not create
 * one (a Windows session without the privilege); the caller then skips the
 * hostile case and says so rather than reporting a pass it did not earn. */
static int make_dir_link(const char *link, const char *target) {
#ifdef _WIN32
    char cmd[2600];
    fp(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1", link, target);
    if (system(cmd) != 0) return 0;
    DWORD a = GetFileAttributesA(link);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return symlink(target, link) == 0;
#endif
}

static int make_file_link(const char *link, const char *target) {
#ifdef _WIN32
    char cmd[2600];
    fp(cmd, sizeof(cmd), "cmd /c mklink \"%s\" \"%s\" >nul 2>&1", link, target);
    if (system(cmd) != 0) return 0;
    DWORD a = GetFileAttributesA(link);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return symlink(target, link) == 0;
#endif
}

static void remove_dir_link(const char *link) {
#ifdef _WIN32
    RemoveDirectoryA(link);
#else
    unlink(link);
#endif
}


static void case_normal_leaf_delete(void) {
    char root[1100], path[1300];
    CHECK(build_save("t_leaf", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_leaf");
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "DATA.BIN"), SR_CD_OK);
    fp(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS00001DATA/DATA.BIN", root);
    CHECK(!exists(path), "in-root leaf must be gone");
    fp(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS00001DATA/PARAM.SFO", root);
    CHECK(exists(path), "sibling files must survive a leaf delete");
    sr_cd_root_close(&r);
    nuke(root);
}

static void case_nonexistent_leaf(void) {
    char root[1100], path[1400];
    CHECK(build_save("t_missing", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_missing");
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    /* Refusal is the contract; the exact refusal code is backend-detail. The
     * Windows verified-handle backend reports one coarse refusal for "the open
     * did not produce a contained object", so it cannot separate "absent" from
     * "outside" without a by-name probe it deliberately does not perform.
     * Callers must not branch on the distinction (see vfs_contained.h). */
    sr_cd_status st = sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "NOPE.BIN");
    CHECK(st == SR_CD_NOT_FOUND || st == SR_CD_NOT_CONTAINED,
          "absent leaf: got %s", sr_cd_status_name(st));
#if defined(SR_CD_BACKEND_POSIX_AT)
    CHECK_ST(st, SR_CD_NOT_FOUND);
#endif
    /* A missing save directory is reported, not papered over. */
    CHECK(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/NOSUCHSAVE", "DATA.BIN") != SR_CD_OK,
          "a missing save directory must not report success");
    CHECK(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/NOSUCHSAVE") != SR_CD_OK,
          "deleting a missing save directory must not report success");
    /* Nothing that does exist was collaterally removed. */
    fp(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS00001DATA/DATA.BIN", root);
    CHECK(exists(path), "a failed delete must leave the real save untouched");
    sr_cd_root_close(&r);
    nuke(root);
}

static void case_directory_is_not_a_file(void) {
    char root[1100], sub[1300];
    CHECK(build_save("t_isdir", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_isdir");
    fp(sub, sizeof(sub), "%s/PSP/SAVEDATA/ULUS00001DATA/SUBDIR", root);
    CHECK(t_mkdir(sub) == 0, "fixture subdir");
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    /* ERASE names a file. A directory must fail closed and stay put -- the
     * distinction comes from the deletion primitive, not a by-name probe. */
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "SUBDIR"), SR_CD_IS_DIRECTORY);
    CHECK(is_dir(sub), "a directory must survive a leaf-delete attempt");
    sr_cd_root_close(&r);
    nuke(root);
}

static void case_populated_directory_delete(void) {
    char root[1100], dir[1300], path[1400];
    CHECK(build_save("t_dir", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_dir");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"), SR_CD_OK);
    CHECK(!exists(dir), "the populated save directory must be gone");
    fp(path, sizeof(path), "%s/PSP/SAVEDATA", root);
    CHECK(is_dir(path), "the SAVEDATA parent must survive");
    sr_cd_root_close(&r);
    nuke(root);
}

/* Many entries: proves the multi-pass drain is not capped by SR_CD_BATCH. */
static void case_large_directory_delete(void) {
    char root[1100], dir[1300], f[1500];
    CHECK(build_save("t_many", "ULUS00001DATA", NULL, 0, root, sizeof(root)), "fixture t_many");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    for (int i = 0; i < SR_CD_BATCH * 3 + 7; i++) {
        fp(f, sizeof(f), "%s/F%03d.BIN", dir, i);
        if (!write_file(f, "x")) { CHECK(0, "fixture entry %d", i); return; }
    }
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"), SR_CD_OK);
    CHECK(!exists(dir), "a directory larger than one enumeration batch must fully drain");
    sr_cd_root_close(&r);
    nuke(root);
}

/* Partial failure: a nested directory blocks the tree delete. The seam removes
 * the plain files it may remove and then refuses, leaving the directory in
 * place rather than reporting a success it did not achieve. */
static void case_partial_failure(void) {
    char root[1100], dir[1300], sub[1400], path[1500];
    CHECK(build_save("t_partial", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_partial");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    fp(sub, sizeof(sub), "%s/SUBDIR", dir);
    CHECK(t_mkdir(sub) == 0, "fixture subdir");
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"), SR_CD_IS_DIRECTORY);
    CHECK(is_dir(dir), "the save directory must survive a refused tree delete");
    CHECK(is_dir(sub), "the nested directory must be left alone");
    fp(path, sizeof(path), "%s/DATA.BIN", dir);
    CHECK(!exists(path), "plain entries the seam may remove are still removed");
    sr_cd_root_close(&r);
    nuke(root);
}

/* Final-component link: the ENTRY inside the verified directory is removed,
 * its target is not. That is the containment rule for links stated positively. */
static void case_final_symlink_entry(int *skipped) {
    char root[1100], dir[1300], link[1500], victim[1300], victim_file[1400];
    CHECK(build_save("t_leaflink", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_leaflink");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    fp(victim, sizeof(victim), "%s/t_leaflink_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/SECRET.TXT", victim);
    CHECK(write_file(victim_file, "keep me"), "fixture victim file");

    fp(link, sizeof(link), "%s/LINK.BIN", dir);
    if (!make_file_link(link, victim_file)) {
        *skipped += 1;
        fprintf(stderr, "SKIP: host refused to create a file link (case_final_symlink_entry)\n");
        nuke(root);
        nuke(victim);
        return;
    }
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "LINK.BIN"), SR_CD_OK);
    CHECK(!exists(link), "the link entry itself must be removed");
    CHECK(exists(victim_file), "the link TARGET outside the root must survive");
    sr_cd_root_close(&r);
    nuke(root);
    nuke(victim);
}

/* Leaf replacement: the name a caller asked to erase is swapped for a link to
 * an outside file before the delete runs. Descriptor-relative deletion removes
 * the link object inside the verified directory; nothing outside is touched. */
static void case_leaf_replacement(int *skipped) {
    char root[1100], dir[1300], leaf[1500], victim[1300], victim_file[1400];
    CHECK(build_save("t_leafswap", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_leafswap");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    fp(victim, sizeof(victim), "%s/t_leafswap_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/SECRET.TXT", victim);
    CHECK(write_file(victim_file, "keep me"), "fixture victim file");

    fp(leaf, sizeof(leaf), "%s/DATA.BIN", dir);
    t_unlink(leaf);
    if (!make_file_link(leaf, victim_file)) {
        *skipped += 1;
        fprintf(stderr, "SKIP: host refused to create a file link (case_leaf_replacement)\n");
        nuke(root);
        nuke(victim);
        return;
    }
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    sr_cd_status st = sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "DATA.BIN");
    CHECK(st == SR_CD_OK || st == SR_CD_NOT_CONTAINED, "swapped leaf: got %s",
          sr_cd_status_name(st));
    CHECK(exists(victim_file), "a swapped leaf must never redirect the delete outside the root");
    sr_cd_root_close(&r);
    nuke(root);
    nuke(victim);
}

/* H1: the LEGACY pathname design, performed for real.
 *
 * stat(path) then unlink(path) with an intermediate component replaced by a
 * link. Both calls re-enter the namespace from the top, so both land in the
 * attacker's directory. The victim file outside the root is destroyed. This is
 * the defect being fixed, demonstrated rather than asserted. */
static int case_legacy_is_redirectable(int *skipped) {
    char root[1100], sd[1200], save[1300], victim[1300], victim_file[1400], through[1500];
    CHECK(build_save("t_legacy", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_legacy");
    fp(sd, sizeof(sd), "%s/PSP/SAVEDATA", root);
    snprintf(save, sizeof(save), "%s/ULUS00001DATA", sd);
    fp(victim, sizeof(victim), "%s/t_legacy_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/DATA.BIN", victim);
    CHECK(write_file(victim_file, "outside the memstick root"), "fixture victim file");

    nuke(save);
    if (!make_dir_link(save, victim)) {
        *skipped += 1;
        fprintf(stderr, "SKIP: host refused to create a directory link (legacy demo)\n");
        nuke(root);
        nuke(victim);
        return 0;
    }
    /* Exactly the pre-fix sequence. */
    fp(through, sizeof(through), "%s/DATA.BIN", save);
    struct stat st;
    int redirected = 0;
    if (stat(through, &st) == 0 && !S_ISDIR(st.st_mode)) {
        if (t_unlink(through) == 0) redirected = 1;
    }
    CHECK(redirected, "the legacy stat->unlink sequence was expected to reach the swapped target");
    CHECK(!exists(victim_file),
          "legacy demo: the outside victim was expected to be destroyed by the by-name unlink");
    remove_dir_link(save);
    nuke(root);
    nuke(victim);
    return redirected;
}

/* H2: the SAME fixture, through the seam. The save-directory component is a
 * link to an outside directory; the seam refuses and the victim survives. */
static void case_intermediate_symlink_escape_blocked(int *skipped) {
    char root[1100], sd[1200], save[1300], victim[1300], victim_file[1400];
    CHECK(build_save("t_escape", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_escape");
    fp(sd, sizeof(sd), "%s/PSP/SAVEDATA", root);
    snprintf(save, sizeof(save), "%s/ULUS00001DATA", sd);
    fp(victim, sizeof(victim), "%s/t_escape_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/DATA.BIN", victim);
    CHECK(write_file(victim_file, "outside the memstick root"), "fixture victim file");

    nuke(save);
    if (!make_dir_link(save, victim)) {
        *skipped += 1;
        fprintf(stderr, "SKIP: host refused to create a directory link (escape case)\n");
        nuke(root);
        nuke(victim);
        return;
    }
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);

    /* ERASE through the swapped component: refused. */
    sr_cd_status leaf_st = sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "DATA.BIN");
    CHECK(leaf_st == SR_CD_NOT_CONTAINED, "swapped save component (leaf): got %s",
          sr_cd_status_name(leaf_st));
    CHECK(exists(victim_file), "the outside victim must survive an ERASE through a swapped link");

    /* DELETE of the whole save directory through the swapped component: also
     * refused, and the outside directory keeps its contents. */
    sr_cd_status dir_st = sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA");
    CHECK(dir_st == SR_CD_NOT_CONTAINED, "swapped save component (tree): got %s",
          sr_cd_status_name(dir_st));
    CHECK(exists(victim_file), "the outside victim must survive a DELETE through a swapped link");
    CHECK(is_dir(victim), "the outside directory itself must survive");

    sr_cd_root_close(&r);
    remove_dir_link(save);
    nuke(root);
    nuke(victim);
}

/* An ANCESTOR of the save directory (PSP/) is the swapped component. The walk
 * opens PSP with O_NOFOLLOW / verifies its final path, so the redirect dies one
 * level higher than the save directory itself. */
static void case_intermediate_ancestor_swap(int *skipped) {
    char root[1100], psp[1200], victim[1300], victim_file[1500], plant[1500];
    CHECK(build_save("t_ancestor", NULL, NULL, 0, root, sizeof(root)), "fixture t_ancestor");
    fp(psp, sizeof(psp), "%s/PSP", root);
    fp(victim, sizeof(victim), "%s/t_ancestor_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(plant, sizeof(plant), "%s/SAVEDATA", victim);
    CHECK(t_mkdir(plant) == 0, "fixture victim SAVEDATA");
    fp(plant, sizeof(plant), "%s/SAVEDATA/ULUS00001DATA", victim);
    CHECK(t_mkdir(plant) == 0, "fixture victim save dir");
    fp(victim_file, sizeof(victim_file), "%s/SAVEDATA/ULUS00001DATA/DATA.BIN", victim);
    CHECK(write_file(victim_file, "outside"), "fixture victim file");

    nuke(psp);
    if (!make_dir_link(psp, victim)) {
        *skipped += 1;
        fprintf(stderr, "SKIP: host refused to create a directory link (ancestor case)\n");
        nuke(root);
        nuke(victim);
        return;
    }
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "DATA.BIN"), SR_CD_NOT_CONTAINED);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"), SR_CD_NOT_CONTAINED);
    CHECK(exists(victim_file), "a swapped ANCESTOR must not redirect the delete");
    sr_cd_root_close(&r);
    remove_dir_link(psp);
    nuke(root);
    nuke(victim);
}

/* Parent replacement, tested the way it is practically testable: the bound
 * root's own directory is renamed away after binding, and a decoy memstick
 * tree takes over the vacated name.
 *
 * The two backends bind different identities, and both bindings are defensible:
 *   POSIX   anchors a DESCRIPTOR, so the binding follows the inode -- the
 *           renamed original is what gets operated on;
 *   Windows anchors the canonical NAME, so the binding follows the configured
 *           memstick path -- whatever now sits at that path is operated on.
 * Neither is an escape: both targets are memstick-shaped roots the operator
 * pointed at. The invariant that must hold on every backend is the one asserted
 * here -- an unrelated directory outside any configured root is never touched,
 * and exactly one of the two candidate saves is affected, never both. */
static void case_parent_replacement(void) {
    char root[1100], moved[1200], decoy[1200];
    char moved_file[1400], decoy_file[1400], victim[1200], victim_file[1400];
    CHECK(build_save("t_parent", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_parent");

    fp(victim, sizeof(victim), "%s/t_parent_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/DATA.BIN", victim);
    CHECK(write_file(victim_file, "unrelated"), "fixture victim file");

    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);

    fp(moved, sizeof(moved), "%s/t_parent_moved", g_tmp);
    nuke(moved);
    if (rename(root, moved) != 0) {
        /* Some hosts refuse to rename a directory that is currently bound;
         * that is a legitimate outcome, not a failure of containment. */
        fprintf(stderr, "NOTE: host refused to rename a bound root (parent replacement)\n");
        sr_cd_root_close(&r);
        nuke(root);
        nuke(victim);
        return;
    }
    fp(moved_file, sizeof(moved_file), "%s/PSP/SAVEDATA/ULUS00001DATA/DATA.BIN", moved);

    /* The decoy takes over the vacated name with an identically shaped save. */
    fp(decoy, sizeof(decoy), "%s", root);
    CHECK(build_save("t_parent", "ULUS00001DATA", SAVE_FILES, 3, decoy, sizeof(decoy)),
          "decoy fixture");
    fp(decoy_file, sizeof(decoy_file), "%s/PSP/SAVEDATA/ULUS00001DATA/DATA.BIN", decoy);
    CHECK(exists(decoy_file), "decoy file");
    CHECK(exists(moved_file), "renamed original file");

    sr_cd_status st = sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "DATA.BIN");
    int hit_moved = !exists(moved_file), hit_decoy = !exists(decoy_file);

    CHECK(exists(victim_file),
          "a renamed root must never redirect a delete onto an unrelated directory");
    if (st == SR_CD_OK) {
        CHECK(hit_moved != hit_decoy,
              "a successful delete must affect exactly one candidate root (moved=%d decoy=%d)",
              hit_moved, hit_decoy);
#if defined(SR_CD_BACKEND_POSIX_AT)
        /* Descriptor anchoring is the stronger binding: the rename cannot move
         * it, so the object that was bound is the object that is destroyed. */
        CHECK(hit_moved, "the descriptor-anchored backend must follow the bound inode");
        CHECK(!hit_decoy, "the descriptor-anchored backend must not follow the vacated name");
#endif
    } else {
        CHECK(!hit_moved && !hit_decoy,
              "a refused delete must destroy nothing (moved=%d decoy=%d)", hit_moved, hit_decoy);
    }
    printf("NOTE: parent replacement -> %s (moved=%d decoy=%d)\n",
           sr_cd_status_name(st), hit_moved, hit_decoy);

    sr_cd_root_close(&r);
    nuke(moved);
    nuke(decoy);
    nuke(victim);
}

/* ---- Layer A: the generic relative-path grammar ------------------------- */

/* Every non-canonical form, refused identically on every host and BEFORE any
 * host destructive primitive is reached.
 *
 * The rooted, repeated-separator and trailing-separator forms are the ones an
 * independent review proved were previously ACCEPTED and silently normalized.
 * They caused no escape (the walk is descriptor anchored) but they left the
 * contract ambiguous, so they are pinned here one form at a time. */
static void case_generic_path_grammar(void) {
    static const char *const rejected[] = {
        "",                       /* empty path */
        "/",                      /* bare separator */
        "/PSP/SAVEDATA/X",        /* ROOTED, forward slash */
        "\\PSP\\SAVEDATA\\X",     /* ROOTED, backslash */
        "//PSP/SAVEDATA/X",       /* rooted twice */
        "PSP//SAVEDATA/X",        /* REPEATED separator */
        "PSP/SAVEDATA//X",        /* repeated separator, deeper */
        "PSP///X",                /* repeated separator, three */
        "PSP/SAVEDATA/X/",        /* TRAILING separator */
        "PSP/SAVEDATA/",          /* trailing separator, shorter */
        "PSP/SAVEDATA//",         /* trailing repeated separator */
        "PSP/SAVEDATA/X\\Y",      /* backslash mid-path: never reinterpreted */
        "PSP\\SAVEDATA/X",        /* backslash as a would-be separator */
        ".",                      /* dot as a whole path */
        "..",                     /* dot-dot as a whole path */
        "PSP/./X",                /* dot component */
        "PSP/../X",               /* dot-dot component */
        "../PSP",                 /* leading dot-dot */
        "PSP/..",                 /* trailing dot-dot */
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        CHECK(!sr_cd_rel_is_canonical(rejected[i]),
              "non-canonical path must be refused: '%s'", rejected[i]);
    }
    CHECK(!sr_cd_rel_is_canonical(NULL), "NULL must be refused");

    static const char *const accepted[] = {
        "X",
        "PSP/SAVEDATA/ULUS00001DATA",
        "a/b/c/d/e",
        "file..bak",              /* dot-dot INSIDE a name is an ordinary name */
        "...",
    };
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        CHECK(sr_cd_rel_is_canonical(accepted[i]),
              "canonical path must be accepted: '%s'", accepted[i]);
    }

    /* Length bound. */
    {
        char toolong[SR_CD_REL_MAX + 8];
        memset(toolong, 'a', sizeof(toolong) - 1);
        toolong[sizeof(toolong) - 1] = '\0';
        CHECK(!sr_cd_rel_is_canonical(toolong), "an over-long path must be refused");
    }
}

/* Every rejected form must reach NO host destructive primitive. Proved by
 * running each one against a live fixture and asserting both the refusal and
 * that every file in the fixture survived it. */
static void case_rejected_paths_touch_nothing(void) {
    char root[1100], data[1400], sfo[1400], icon[1400], dir[1300];
    static const char *const rejected[] = {
        "/PSP/SAVEDATA/ULUS00001DATA",
        "\\PSP\\SAVEDATA\\ULUS00001DATA",
        "PSP//SAVEDATA/ULUS00001DATA",
        "PSP/SAVEDATA/ULUS00001DATA/",
        "PSP/SAVEDATA//ULUS00001DATA",
        "PSP/./SAVEDATA/ULUS00001DATA",
        "PSP/../SAVEDATA/ULUS00001DATA",
        "",
        "/",
        "..",
    };
    CHECK(build_save("t_reject", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_reject");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    fp(data, sizeof(data), "%s/DATA.BIN", dir);
    fp(sfo, sizeof(sfo), "%s/PARAM.SFO", dir);
    fp(icon, sizeof(icon), "%s/ICON0.PNG", dir);

    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        CHECK_ST(sr_cd_delete_leaf(&r, rejected[i], "DATA.BIN"), SR_CD_INVALID_PATH);
        CHECK_ST(sr_cd_delete_dir_shallow(&r, rejected[i]), SR_CD_INVALID_PATH);
        CHECK(exists(data) && exists(sfo) && exists(icon) && is_dir(dir),
              "a refused path must destroy nothing: '%s'", rejected[i]);
    }
    /* The same rule applies to the leaf NAME, not just the directory path. */
    static const char *const bad_leaves[] = { "", ".", "..", "a/b", "PSP/DATA.BIN" };
    for (size_t i = 0; i < sizeof(bad_leaves) / sizeof(bad_leaves[0]); i++) {
        CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", bad_leaves[i]),
                 SR_CD_INVALID_PATH);
        CHECK(exists(data) && exists(sfo) && exists(icon),
              "a refused leaf must destroy nothing: '%s'", bad_leaves[i]);
    }
    sr_cd_root_close(&r);
    nuke(root);
}

/* ---- the Layer A / Layer C boundary ------------------------------------ */

/* The generic grammar must carry NO host's filename taboos, and the host layer
 * must carry exactly its own. Getting this backwards in either direction is a
 * defect: a Windows-shaped generic API breaks future hosts, and a POSIX backend
 * that imports Win32 taboos makes legitimately named files undeletable. */
static void case_policy_layer_boundary(void) {
    /* Names that are ordinary on POSIX and hazardous only on Win32. The GENERIC
     * layer must accept every one of them -- it knows nothing about Win32. */
    static const char *const win32_only_hazards[] = {
        "NUL", "CON", "COM5", "LPT3", "AUX", "PRN",  /* DOS device aliases */
        "name.",                                     /* trailing dot: Win32 strips it */
        "name ",                                     /* trailing space: likewise */
        "a:stream",                                  /* Win32 ADS selector */
        "star*", "quest?", "pipe|", "lt<", "gt>", "quote\"",
        "back\\slash",                               /* separator on Win32 only */
    };
    for (size_t i = 0; i < sizeof(win32_only_hazards) / sizeof(win32_only_hazards[0]); i++) {
        const char *n = win32_only_hazards[i];
        CHECK(sr_cd_component_is_generic(n, strlen(n)),
              "generic grammar must NOT encode Win32 policy, but refused '%s'", n);
    }

    /* ...and the HOST layer must make exactly the opposite call, per host. */
    for (size_t i = 0; i < sizeof(win32_only_hazards) / sizeof(win32_only_hazards[0]); i++) {
        const char *n = win32_only_hazards[i];
        int host_ok = sr_cd_component_is_host_ok(n, strlen(n));
#if defined(SR_CD_BACKEND_WINDOWS)
        CHECK(!host_ok, "the Win32 host layer must refuse '%s'", n);
#elif defined(SR_CD_BACKEND_POSIX_AT)
        CHECK(host_ok, "the POSIX host layer must accept the ordinary filename '%s'", n);
#else
        CHECK(!host_ok, "a backend that performs nothing must accept nothing ('%s')", n);
#endif
    }

    /* Structural rules are generic and identical everywhere, host layer or not. */
    static const char *const structural[] = { "", ".", "..", "a/b" };
    for (size_t i = 0; i < sizeof(structural) / sizeof(structural[0]); i++) {
        CHECK(!sr_cd_component_is_generic(structural[i], strlen(structural[i])),
              "structural rules are generic: '%s' must be refused", structural[i]);
        CHECK(!sr_cd_name_is_acceptable(structural[i], strlen(structural[i])),
              "an unacceptable component must be refused whichever layer catches it");
    }

    /* A component of exactly SR_CD_NAME_MAX - 1 bytes is legal; one more is not. */
    {
        char name[SR_CD_NAME_MAX + 4];
        memset(name, 'n', SR_CD_NAME_MAX - 1);
        name[SR_CD_NAME_MAX - 1] = '\0';
        CHECK(sr_cd_component_is_generic(name, SR_CD_NAME_MAX - 1), "max-length name is legal");
        memset(name, 'n', SR_CD_NAME_MAX);
        name[SR_CD_NAME_MAX] = '\0';
        CHECK(!sr_cd_component_is_generic(name, SR_CD_NAME_MAX), "over-long name is refused");
    }
}

/* The boundary, executed end to end: a save directory holding a file whose name
 * this HOST allows but Win32 would not must still be fully deletable here. On
 * Windows the same name cannot be created in the first place, so the case
 * asserts the refusal instead. Either way the seam never leaves a save
 * directory stuck because of another host's grammar. */
static void case_host_policy_end_to_end(void) {
    char root[1100], dir[1300], odd[1500];
    CHECK(build_save("t_hostpol", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_hostpol");
    fp(dir, sizeof(dir), "%s/PSP/SAVEDATA/ULUS00001DATA", root);

    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);

#if defined(SR_CD_BACKEND_POSIX_AT)
    /* "NUL" and "odd." are ordinary POSIX filenames. A save directory holding
     * them must drain completely -- refusing them here would leave the
     * directory permanently undeletable, which is the bug this layering fixes. */
    fp(odd, sizeof(odd), "%s/NUL", dir);
    CHECK(write_file(odd, "ordinary here"), "fixture NUL entry");
    fp(odd, sizeof(odd), "%s/odd.", dir);
    CHECK(write_file(odd, "ordinary here"), "fixture trailing-dot entry");
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "NUL"), SR_CD_OK);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"), SR_CD_OK);
    CHECK(!exists(dir), "a directory holding host-legal odd names must fully drain");
#elif defined(SR_CD_BACKEND_WINDOWS)
    /* Win32 would reinterpret both names, so the host layer refuses them and
     * nothing reaches a Win32 destructive call. */
    (void)odd;
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "NUL"), SR_CD_INVALID_PATH);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "odd."), SR_CD_INVALID_PATH);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"), SR_CD_OK);
    CHECK(!exists(dir), "an ordinary save directory must still delete");
#else
    (void)odd;
#endif
    sr_cd_root_close(&r);
    nuke(root);
}

/* ---- final-directory object identity ----------------------------------- */

/* Deterministic replacement of the save directory, driven by the seam's own
 * test hook rather than by a timing race.
 *
 * The hook fires after the target has been emptied and before its name is
 * re-resolved -- exactly where a hostile actor would have to act. The callback
 * renames the drained directory aside and drops a DIFFERENT in-root directory
 * into its place. The required outcome is the same on every backend: the
 * replacement survives, the operation fails closed, and nothing outside is
 * touched. */
typedef struct {
    char target[1400];   /* the save directory's path */
    char aside[1400];    /* where the drained original is parked */
    char decoy_mark[1500]; /* a file inside the substitute, to prove it survived */
    int populate;          /* 0 leaves the substitute EMPTY -- see below */
    int fired;
} SwapCtx;

#if defined(SR_CD_TEST_HOOKS)
static void swap_target_dir(void *vctx) {
    SwapCtx *c = (SwapCtx *)vctx;
    c->fired++;
    if (rename(c->target, c->aside) != 0) return;
    if (t_mkdir(c->target) != 0) return;
    /* An EMPTY substitute is the dangerous shape: AT_REMOVEDIR refuses a
     * non-empty directory for free, so only an empty one can actually be
     * destroyed by an unprotected removal. Cases that want to prove the
     * identity check is load bearing leave it empty. */
    if (c->populate) (void)write_file(c->decoy_mark, "substitute: must survive");
}
#endif

static void case_final_dir_replacement(void) {
#if !defined(SR_CD_TEST_HOOKS)
    fprintf(stderr, "SKIP: built without SR_CD_TEST_HOOKS (final-dir replacement)\n");
#else
    char root[1100], victim[1200], victim_file[1400];
    SwapCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    CHECK(build_save("t_swap", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_swap");
    fp(ctx.target, sizeof(ctx.target), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    fp(ctx.aside, sizeof(ctx.aside), "%s/PSP/SAVEDATA/PARKED", root);
    fp(ctx.decoy_mark, sizeof(ctx.decoy_mark), "%s/KEEPME.BIN", ctx.target);
    ctx.populate = 1;

    /* An unrelated directory outside the root, to prove containment separately. */
    fp(victim, sizeof(victim), "%s/t_swap_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/DATA.BIN", victim);
    CHECK(write_file(victim_file, "unrelated"), "fixture victim file");

    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    sr_cd_test_hooks_clear();
    sr_cd_test_hook_set(SR_CD_HOOK_AFTER_DRAIN, swap_target_dir, &ctx);
    sr_cd_status st = sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA");
    sr_cd_test_hooks_clear();

    CHECK(ctx.fired == 1, "the replacement hook must have fired exactly once (got %d)",
          ctx.fired);

    /* Required of EVERY backend, whatever its object-identity strength: the
     * substitute is not the bound object, so it is never destroyed, and nothing
     * outside the root is reachable at all. */
    CHECK(exists(ctx.decoy_mark),
          "the SUBSTITUTE object must survive: it is not the object that was bound");
    CHECK(is_dir(ctx.target), "the substitute directory itself must survive");
    CHECK(exists(victim_file), "nothing outside the root may be touched");

#if defined(SR_CD_BACKEND_POSIX_AT)
    /* POSIX cannot address the directory by descriptor, so it DETECTS the
     * substitution -- bound (st_dev, st_ino) against a freshly opened
     * descriptor -- and fails closed having removed nothing. The bound object,
     * parked aside by the hook, is therefore still there. */
    CHECK_ST(st, SR_CD_IDENTITY_CHANGED);
    CHECK(is_dir(ctx.aside), "a fail-closed refusal must leave the bound object intact");
#elif defined(SR_CD_BACKEND_WINDOWS)
    /* Windows never re-resolves the name at all: the disposition rides the
     * handle bound before the drain, so the substitution is not something to be
     * detected -- it is irrelevant. The bound object is removed even though the
     * hook renamed it aside, which is exactly what OBJECT_BOUND means and is a
     * strictly stronger outcome than failing closed. */
    CHECK_ST(st, SR_CD_OK);
    CHECK(!exists(ctx.aside),
          "the object-bound disposition must remove the BOUND object, wherever its name went");
#endif

    sr_cd_root_close(&r);
    nuke(ctx.aside);
    nuke(root);
    nuke(victim);
#endif
}

/* The residual window, documented by execution rather than by assertion.
 *
 * A substitution performed AFTER the identity confirmation cannot be detected
 * by any POSIX.1-2008 sequence -- there is no atomic compare-and-remove. This
 * case drives the hook at that exact point and records what actually happens,
 * so the bound stated in vfs_contained.h is measured rather than assumed. What
 * it ASSERTS is only the part that is genuinely guaranteed: whatever happens
 * stays inside the root, and a NON-EMPTY substitute is refused outright. */
static void case_residual_window_is_bounded(void) {
#if !defined(SR_CD_TEST_HOOKS)
    fprintf(stderr, "SKIP: built without SR_CD_TEST_HOOKS (residual window)\n");
#else
    char root[1100], victim[1200], victim_file[1400];
    SwapCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    CHECK(build_save("t_resid", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_resid");
    fp(ctx.target, sizeof(ctx.target), "%s/PSP/SAVEDATA/ULUS00001DATA", root);
    fp(ctx.aside, sizeof(ctx.aside), "%s/PSP/SAVEDATA/PARKED", root);
    /* The substitute is NON-EMPTY on purpose: that is the half of the residual
     * the kernel closes for us, and it is the half worth asserting. */
    fp(ctx.decoy_mark, sizeof(ctx.decoy_mark), "%s/KEEPME.BIN", ctx.target);
    ctx.populate = 1;

    fp(victim, sizeof(victim), "%s/t_resid_victim", g_tmp);
    nuke(victim);
    CHECK(t_mkdir(victim) == 0, "fixture victim dir");
    fp(victim_file, sizeof(victim_file), "%s/DATA.BIN", victim);
    CHECK(write_file(victim_file, "unrelated"), "fixture victim file");

    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    sr_cd_test_hooks_clear();
    sr_cd_test_hook_set(SR_CD_HOOK_AFTER_CONFIRM, swap_target_dir, &ctx);
    sr_cd_status st = sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA");
    sr_cd_test_hooks_clear();

    CHECK(ctx.fired == 1, "the residual hook must have fired exactly once (got %d)", ctx.fired);
    /* Guaranteed regardless of backend: a NON-EMPTY substitute is never
     * removed, and nothing outside the root is reachable at all. */
    CHECK(exists(ctx.decoy_mark),
          "a non-empty substitute must survive even inside the residual window");
    CHECK(exists(victim_file), "nothing outside the root may be touched");
    printf("NOTE: residual window (post-confirm swap) -> %s\n", sr_cd_status_name(st));

    sr_cd_root_close(&r);
    nuke(ctx.aside);
    nuke(root);
    nuke(victim);
#endif
}

/* The guarantee table must describe this build, not an aspiration. */
static void case_guarantees_are_declared(void) {
    unsigned g = sr_cd_backend_guarantees();
    CHECK((g & SR_CD_GUARANTEE_CONTAINMENT) != 0, "containment must be guaranteed");
    CHECK((g & SR_CD_GUARANTEE_NO_LINK_TRAVERSAL) != 0, "link non-traversal must be guaranteed");
    CHECK((g & SR_CD_GUARANTEE_TYPE_ENFORCED) != 0, "type enforcement must be guaranteed");
#if defined(SR_CD_BACKEND_WINDOWS)
    CHECK((g & SR_CD_GUARANTEE_DIR_OBJECT_BOUND) != 0,
          "the verified-handle backend does bind the directory object");
#elif defined(SR_CD_BACKEND_POSIX_AT)
    CHECK((g & SR_CD_GUARANTEE_DIR_OBJECT_BOUND) == 0,
          "POSIX.1-2008 cannot bind the directory object and must not claim to");
#endif
}

/* Name discipline, end to end: a refused name reaches no host primitive. */
static void case_name_discipline(void) {
    char root[1100];
    CHECK(build_save("t_names", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
          "fixture t_names");
    sr_cd_root r;
    CHECK_ST(sr_cd_root_open(root, &r), SR_CD_OK);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", ".."), SR_CD_INVALID_PATH);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "a/b"), SR_CD_INVALID_PATH);
    CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/../SAVEDATA/X", "DATA.BIN"), SR_CD_INVALID_PATH);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, ".."), SR_CD_INVALID_PATH);
    CHECK_ST(sr_cd_delete_dir_shallow(&r, ""), SR_CD_INVALID_PATH);
    /* The root itself is never a deletable tree: it has no relative path. */
    CHECK_ST(sr_cd_delete_dir_shallow(&r, "/"), SR_CD_INVALID_PATH);
    sr_cd_root_close(&r);
    nuke(root);
}

static void case_rel_split_unit(void) {
    char parent[64], last[64];
    CHECK(sr_cd_rel_split("PSP/SAVEDATA/X", parent, sizeof(parent), last, sizeof(last)),
          "split of a three-component path");
    CHECK(strcmp(parent, "PSP/SAVEDATA") == 0, "parent got '%s'", parent);
    CHECK(strcmp(last, "X") == 0, "last got '%s'", last);
    CHECK(sr_cd_rel_split("ONLY", parent, sizeof(parent), last, sizeof(last)), "single component");
    CHECK(parent[0] == '\0', "single component parent must be empty, got '%s'", parent);
    CHECK(strcmp(last, "ONLY") == 0, "last got '%s'", last);
    CHECK(!sr_cd_rel_split("", parent, sizeof(parent), last, sizeof(last)), "empty must reject");
    CHECK(!sr_cd_rel_split("A/../B", parent, sizeof(parent), last, sizeof(last)),
          "traversal must reject");
    CHECK(!sr_cd_rel_split("/A/B", parent, sizeof(parent), last, sizeof(last)),
          "a rooted path must reject");
    CHECK(!sr_cd_rel_split("A//B", parent, sizeof(parent), last, sizeof(last)),
          "a repeated separator must reject");
    CHECK(!sr_cd_rel_split("A/B/", parent, sizeof(parent), last, sizeof(last)),
          "a trailing separator must reject");
}

#endif /* !SR_CD_FORCE_UNSUPPORTED_BACKEND */

int main(void) {
    int skipped = 0;

    printf("vfs_contained selftest: backend=%s contained=%d\n",
           sr_cd_backend_name(), sr_cd_backend_is_contained());

    if (!make_sandbox()) { fprintf(stderr, "cannot create sandbox\n"); return 2; }

#if defined(SR_CD_FORCE_UNSUPPORTED_BACKEND)
    /* Unsupported-host contract: every entry point refuses and nothing is
     * destroyed. No pathname fallback exists to fall back to. */
    {
        char root[1100], path[1400];
        CHECK(build_save("t_unsup", "ULUS00001DATA", SAVE_FILES, 3, root, sizeof(root)),
              "fixture t_unsup");
        sr_cd_root r;
        CHECK_ST(sr_cd_root_open(root, &r), SR_CD_UNSUPPORTED_HOST);
        CHECK_ST(sr_cd_delete_leaf(&r, "PSP/SAVEDATA/ULUS00001DATA", "DATA.BIN"),
                 SR_CD_UNSUPPORTED_HOST);
        CHECK_ST(sr_cd_delete_dir_shallow(&r, "PSP/SAVEDATA/ULUS00001DATA"),
                 SR_CD_UNSUPPORTED_HOST);
        fp(path, sizeof(path), "%s/PSP/SAVEDATA/ULUS00001DATA/DATA.BIN", root);
        CHECK(exists(path), "an unsupported host must destroy nothing");
        CHECK(sr_cd_backend_is_contained() == 0, "the unsupported backend must not claim containment");
        sr_cd_root_close(&r);
        nuke(root);
    }
#else
    case_guarantees_are_declared();
    case_generic_path_grammar();
    case_rel_split_unit();
    case_rejected_paths_touch_nothing();
    case_policy_layer_boundary();
    case_host_policy_end_to_end();
    case_final_dir_replacement();
    case_residual_window_is_bounded();
    case_normal_leaf_delete();
    case_nonexistent_leaf();
    case_directory_is_not_a_file();
    case_populated_directory_delete();
    case_large_directory_delete();
    case_partial_failure();
    case_name_discipline();
    case_final_symlink_entry(&skipped);
    case_leaf_replacement(&skipped);
    case_parent_replacement();

    {
        int legacy_redirected = case_legacy_is_redirectable(&skipped);
        case_intermediate_symlink_escape_blocked(&skipped);
        case_intermediate_ancestor_swap(&skipped);
        if (legacy_redirected)
            printf("vfs_contained selftest: legacy pathname design confirmed redirectable; "
                   "seam refused the identical fixture\n");
    }
#endif

    t_rmdir(g_tmp);

    printf("vfs_contained selftest: %d checks, %d skipped hostile case(s)\n", g_checks, skipped);
    if (g_failed) {
        fprintf(stderr, "vfs_contained selftest: FAILED\n");
        return 1;
    }
    printf("vfs_contained selftest: OK\n");
    return 0;
}
