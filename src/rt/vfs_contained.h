// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Contained destructive-filesystem seam (host-neutral).
 *
 * WHY THIS EXISTS
 * ---------------
 * Generic PSP/savedata logic must be able to say
 *
 *     "delete this validated contained object/tree beneath this trusted host root"
 *
 * without naming a host primitive. It must NOT say "call unlink()" (which
 * assumes POSIX) or "call SetFileInformationByHandle()" (which assumes Win32).
 * Nakagawa targets substantially more hosts than the Windows PC it is currently
 * developed on; Windows is a development host, not the architectural target.
 *
 * So this header defines ONE narrow semantic contract plus per-host backends.
 * Adding a desktop, mobile or handheld host later means implementing the entry
 * points below for that host -- it must never mean editing savedata semantics
 * again.
 *
 * THE CONTRACT
 * ------------
 *   sr_cd_root_open       bind a trusted host root ONCE, by whatever identity
 *                         the host can pin (a descriptor, a canonical name...)
 *   sr_cd_delete_leaf     delete ONE non-directory object named by a leaf name
 *                         inside a root-relative directory
 *   sr_cd_delete_dir_shallow
 *                         delete every non-directory entry of a root-relative
 *                         directory and then the directory itself
 *   sr_cd_root_close      release the binding
 *
 * TWO SEPARATE GUARANTEES
 * -----------------------
 * These are different promises and are deliberately not conflated. Query them
 * with sr_cd_backend_guarantees().
 *
 *   CONTAINMENT     no operation destroys anything outside the bound root, and
 *                   no link/reparse point is ever traversed by a destructive
 *                   step. Guaranteed by every backend, unconditionally. This is
 *                   the security property.
 *
 *   OBJECT IDENTITY the object destroyed is the exact object that was bound and
 *                   verified, not whatever answers to its name at the instant
 *                   of removal. This is a correctness property and it is NOT
 *                   uniformly available:
 *
 *                     Windows  GUARANTEED for the save directory. The
 *                              disposition is set on the verified HANDLE, so no
 *                              second name resolution exists to lose.
 *                     POSIX    NOT guaranteed, because POSIX.1-2008 has no
 *                              descriptor-addressed directory removal and no
 *                              atomic compare-and-remove -- every removal
 *                              primitive (unlinkat, renameat) is name
 *                              addressed. The backend instead BINDS the
 *                              target's (st_dev, st_ino) and re-confirms it
 *                              immediately before removal, so a replacement
 *                              present at confirmation time is DETECTED and the
 *                              operation fails closed having removed nothing.
 *                              The residual window is stated exactly at
 *                              sr_cd_delete_dir_shallow.
 *
 * Nothing here claims an object-bound delete it cannot perform. A caller that
 * needs the stronger promise must test for it rather than assume it.
 *
 * PATH AND NAME POLICY IS LAYERED
 * -------------------------------
 * Three different questions, kept apart on purpose so that a generic API does
 * not become Windows-shaped by accident:
 *
 *   A. GENERIC RELATIVE-PATH GRAMMAR -- sr_cd_rel_is_canonical /
 *      sr_cd_component_is_generic, right here. Containment structure only:
 *      separators, rooted forms, empty components, dot and dot-dot, length.
 *      This layer encodes NO host's filename taboos.
 *
 *   B. PSP / TITLE COMPONENT POLICY -- owned by the savedata-facing caller
 *      (savedata.c path_sanitize / save_rel). A PSP save name must be portable
 *      between this runtime, PPSSPP and real hardware, so savedata applies the
 *      strictest common grammar to guest-chosen names. That is a title-level
 *      product decision and it does not belong in this seam.
 *
 *   C. HOST-BACKEND FILENAME RESTRICTIONS -- sr_cd_component_is_host_ok,
 *      defined by each backend. Win32 must refuse reserved device names, ADS
 *      colons, wildcards and trailing dot/space because Win32 would otherwise
 *      REINTERPRET them. POSIX has no such grammar and therefore adds nothing:
 *      imposing Win32's taboos there would make a legitimately named file
 *      undeletable, which is a bug, not extra safety.
 *
 * Every layer fails closed. A name rejected by any of them reaches no host
 * destructive primitive at all.
 *
 * STRICT-ISO NOTE
 * ---------------
 * The POSIX backend needs POSIX.1-2008 declarations (openat, fdopendir,
 * unlinkat). Under a strict -std=cNN build glibc hides those unless a feature
 * test macro is set, so this header sets _POSIX_C_SOURCE itself when the
 * translation unit has selected no feature profile at all -- which is only
 * effective if this header is included BEFORE any system header. Consumers
 * therefore include "vfs_contained.h" first (see savedata.c). If the
 * declarations are unavailable anyway, the capability probe below refuses to
 * select a backend and the build stops.
 */

#ifndef SR_VFS_CONTAINED_H
#define SR_VFS_CONTAINED_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE) && \
    !defined(_DEFAULT_SOURCE) && !defined(_XOPEN_SOURCE) && !defined(_BSD_SOURCE) && \
    !defined(_DARWIN_C_SOURCE) && !defined(_NETBSD_SOURCE) && !defined(__BSD_VISIBLE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "vfs_path.h"

/* ---- backend selection ------------------------------------------------- */

/* SR_CD_FORCE_UNSUPPORTED_BACKEND compiles the fail-closed backend on a host
 * that does have one. It exists so the unsupported-host contract is executable
 * evidence rather than a claim; production builds never define it. */
#if defined(SR_CD_FORCE_UNSUPPORTED_BACKEND)
#define SR_CD_BACKEND_NONE 1
#elif defined(_WIN32)
#define SR_CD_BACKEND_WINDOWS 1
#include <dirent.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
/* Capability probe, not a platform guess: a host qualifies for the
 * descriptor-relative backend only when it actually publishes the anchoring
 * primitives the contract needs. Anything missing falls through to the
 * fail-closed backend instead of degrading to pathname deletion. */
#if defined(O_DIRECTORY) && defined(O_NOFOLLOW) && defined(AT_REMOVEDIR) && \
    defined(AT_SYMLINK_NOFOLLOW)
#define SR_CD_BACKEND_POSIX_AT 1
#else
#define SR_CD_BACKEND_NONE 1
#endif
#endif

#if !defined(SR_CD_BACKEND_WINDOWS) && !defined(SR_CD_BACKEND_POSIX_AT) && \
    !defined(SR_CD_BACKEND_NONE)
#define SR_CD_BACKEND_NONE 1
#endif

/* Losing containment must never be an accident. If no backend was selected the
 * build stops here, so a host -- or a feature-profile mistake that hides the
 * POSIX.1-2008 declarations -- cannot quietly demote savedata deletion to
 * fail-closed without somebody deciding that. Shipping a host with no backend
 * is legal, but it has to be stated: define SR_CD_ALLOW_UNSUPPORTED_HOST. */
#if defined(SR_CD_BACKEND_NONE) && !defined(SR_CD_FORCE_UNSUPPORTED_BACKEND) && \
    !defined(SR_CD_ALLOW_UNSUPPORTED_HOST)
#error "vfs_contained.h: no contained-delete backend for this host. Implement one; or, if the host is POSIX, build with a POSIX.1-2008 feature profile (-std=gnu11 or -D_POSIX_C_SOURCE=200809L); or define SR_CD_ALLOW_UNSUPPORTED_HOST to accept fail-closed savedata deletion deliberately."
#endif

/* ...and having stated it, keep stating it. SR_CD_ALLOW_UNSUPPORTED_HOST is a
 * port bring-up escape hatch: it turns savedata deletion into a permanent
 * no-op. Every translation unit compiled with it says so out loud, so it
 * cannot drift into a shipping configuration unnoticed. Two further gates back
 * this up in tools/test_savedata_spans.py: no build input in this repository
 * may define the macro, and this warning must remain unconditional. */
#if defined(SR_CD_ALLOW_UNSUPPORTED_HOST) && !defined(SR_CD_FORCE_UNSUPPORTED_BACKEND)
#warning "SR_CD_ALLOW_UNSUPPORTED_HOST: savedata deletion is DISABLED (fail-closed) in this build. Port bring-up / test configuration only -- this must never ship."
#endif

/* O_CLOEXEC is POSIX.1-2008 but keep the build alive on hosts that omit it;
 * descriptor leakage across exec is not part of the containment contract. */
#if defined(SR_CD_BACKEND_POSIX_AT) && !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

/* A root-RELATIVE canonical path. PSP-shaped by construction
 * ("PSP/SAVEDATA/<name>" is 30-odd bytes); the bound keeps the walk buffers
 * small enough that this seam costs an HLE coroutine almost nothing. */
#define SR_CD_REL_MAX 512
/* One path component. */
#define SR_CD_NAME_MAX 256
/* A backend-constructed HOST path. Only the Windows backend builds one; the
 * POSIX backend never materializes a full pathname at all. */
#define SR_CD_HOST_PATH_MAX 4096
/* One enumeration batch and the pass ceiling for a shallow directory delete.
 * Entries are collected, the enumeration is closed, and only then are names
 * unlinked relative to the retained directory descriptor -- readdir() behaviour
 * while entries are being removed is unspecified, so the delete never depends
 * on it. Passes repeat while progress is made, so an arbitrarily large
 * directory still drains; the batch is sized for a PSP save directory (a
 * handful of files), not for one-pass completion. */
#define SR_CD_BATCH 16
#define SR_CD_MAX_PASSES 4096

typedef enum sr_cd_status {
    SR_CD_OK = 0,
    SR_CD_UNSUPPORTED_HOST, /* no backend implements the containment contract */
    SR_CD_INVALID_PATH,     /* not a canonical relative path, or not a name this host accepts */
    SR_CD_NOT_FOUND,        /* the named object does not exist */
    SR_CD_NOT_CONTAINED,    /* escaped the root, or a link/mount stood in the way */
    SR_CD_IS_DIRECTORY,     /* a directory was named where a file was required */
    SR_CD_NOT_EMPTY,        /* the tree still held entries this seam may not remove */
    SR_CD_IDENTITY_CHANGED, /* the name stopped resolving to the bound object; nothing removed */
    SR_CD_IO_ERROR          /* the host refused the operation */
} sr_cd_status;

/* CALLER CONTRACT: treat EVERY non-OK status as failure.
 *
 * The distinctions above exist for diagnostics and for tests, not for control
 * flow. Backends legitimately differ in how precisely they can classify a
 * refusal -- the Windows verified-handle route cannot separate "absent" from
 * "outside the root" without a by-name probe it deliberately does not perform,
 * so it reports SR_CD_NOT_CONTAINED where POSIX reports SR_CD_NOT_FOUND. No
 * unsafe probe will ever be added merely to normalize that variance. */

/* What this build actually promises. See "TWO SEPARATE GUARANTEES" above. */
#define SR_CD_GUARANTEE_CONTAINMENT 0x1u       /* nothing outside the bound root is destroyed */
#define SR_CD_GUARANTEE_NO_LINK_TRAVERSAL 0x2u /* a link object may be removed, its target never */
#define SR_CD_GUARANTEE_TYPE_ENFORCED 0x4u     /* a file request can never remove a directory */
#define SR_CD_GUARANTEE_DIR_OBJECT_BOUND 0x8u  /* the save directory removed IS the bound object */

typedef struct sr_cd_root {
#if defined(SR_CD_BACKEND_WINDOWS)
    wchar_t canonical[MAX_PATH * 2];
    char prefix[SR_CD_HOST_PATH_MAX];
#elif defined(SR_CD_BACKEND_POSIX_AT)
    int fd;
#else
    int unused;
#endif
} sr_cd_root;

static inline const char *sr_cd_backend_name(void) {
#if defined(SR_CD_BACKEND_WINDOWS)
    return "windows-verified-handle";
#elif defined(SR_CD_BACKEND_POSIX_AT)
    return "posix-descriptor-relative";
#else
    return "unsupported";
#endif
}

static inline unsigned sr_cd_backend_guarantees(void) {
#if defined(SR_CD_BACKEND_WINDOWS)
    /* The directory disposition is set on the verified handle and no second
     * name resolution follows it, so the save-directory removal is object bound
     * with no residual window. */
    return SR_CD_GUARANTEE_CONTAINMENT | SR_CD_GUARANTEE_NO_LINK_TRAVERSAL |
           SR_CD_GUARANTEE_TYPE_ENFORCED | SR_CD_GUARANTEE_DIR_OBJECT_BOUND;
#elif defined(SR_CD_BACKEND_POSIX_AT)
    /* DIR_OBJECT_BOUND is deliberately absent: POSIX.1-2008 cannot express a
     * descriptor-addressed directory removal. Detection is implemented and a
     * detected replacement fails closed, but detection is not a guarantee. */
    return SR_CD_GUARANTEE_CONTAINMENT | SR_CD_GUARANTEE_NO_LINK_TRAVERSAL |
           SR_CD_GUARANTEE_TYPE_ENFORCED;
#else
    return 0u;
#endif
}

/* 1 when this build can meet the containment contract. Callers use it for
 * diagnostics only: every entry point already fails closed on its own. */
static inline int sr_cd_backend_is_contained(void) {
    return (sr_cd_backend_guarantees() & SR_CD_GUARANTEE_CONTAINMENT) != 0u;
}

static inline const char *sr_cd_status_name(sr_cd_status s) {
    switch (s) {
        case SR_CD_OK: return "ok";
        case SR_CD_UNSUPPORTED_HOST: return "unsupported-host";
        case SR_CD_INVALID_PATH: return "invalid-path";
        case SR_CD_NOT_FOUND: return "not-found";
        case SR_CD_NOT_CONTAINED: return "not-contained";
        case SR_CD_IS_DIRECTORY: return "is-directory";
        case SR_CD_NOT_EMPTY: return "not-empty";
        case SR_CD_IDENTITY_CHANGED: return "identity-changed";
        case SR_CD_IO_ERROR: return "io-error";
    }
    return "unknown";
}

/* ======================================================================== */
/* Layer A: generic relative-path grammar. Host-independent by construction. */
/* ======================================================================== */

/* One path component, judged by the GENERIC grammar alone.
 *
 * Only what can change the STRUCTURAL meaning of a path is refused: the empty
 * name, an over-long name, "." and "..", and the canonical separator.
 *
 * Note what is deliberately NOT refused here, because none of it is a
 * containment question: a backslash, a control byte, a trailing dot, a reserved
 * device name. On POSIX every one of those is an ordinary filename byte or an
 * ordinary filename, and refusing them at this layer would make a legitimately
 * named save directory permanently undeletable -- a bug wearing the costume of
 * extra safety. They belong to Layer C (sr_cd_component_is_host_ok), which the
 * Win32 backend implements because Win32 really does reinterpret them. */
static inline int sr_cd_component_is_generic(const char *c, size_t len) {
    if (!c || len == 0 || len >= SR_CD_NAME_MAX) return 0;
    if (len == 1 && c[0] == '.') return 0;
    if (len == 2 && c[0] == '.' && c[1] == '.') return 0;
    for (size_t i = 0; i < len; i++) {
        if (c[i] == '/') return 0;  /* the one canonical separator */
        if (c[i] == '\0') return 0; /* defensive: callers pass counted spans */
    }
    return 1;
}

/* A canonical root-relative path:
 *
 *     rel := component ( "/" component )*
 *
 * '/' is THE separator of this seam, on every host. Refused with no host
 * variation whatsoever:
 *
 *     ""                     the empty path
 *     "/PSP/..."             a rooted path, forward slash
 *     "\PSP\..."             a rooted path, backslash
 *     "PSP/SAVEDATA/"        a trailing separator
 *     "PSP//SAVEDATA"        a repeated separator / empty component
 *     "PSP/./X", "PSP/../X"  a dot or dot-dot component
 *     "."  ".."              likewise as a whole path
 *     anything containing a backslash, anywhere
 *
 * The backslash rule is the one that has to be stated rather than assumed. A
 * backslash is a separator on Win32 and an ordinary filename byte on POSIX, so
 * accepting one in a PATH would make the same string name different objects on
 * different hosts -- the seam would have a host-dependent grammar, which is
 * precisely what it exists to prevent. Rather than silently adopt one host's
 * answer, the generic grammar refuses the character outright at the path level
 * on every host. (Whether a backslash may appear inside a single component
 * NAME, to which no path parsing is applied, is a Layer C question: POSIX says
 * yes, Win32 says no.)
 *
 * Rooted, repeated-separator and trailing-separator forms were previously
 * accepted and silently normalized. They caused no escape -- the walk is
 * descriptor anchored -- but they left the contract ambiguous and would have
 * been a live hazard for any future backend that concatenates rel onto a host
 * root. They are now refused outright. */
static inline int sr_cd_rel_is_canonical(const char *rel) {
    if (!rel) return 0;
    size_t n = strlen(rel);
    if (n == 0 || n >= SR_CD_REL_MAX) return 0;
    if (rel[0] == '/' || rel[0] == '\\') return 0;         /* rooted */
    if (rel[n - 1] == '/' || rel[n - 1] == '\\') return 0; /* trailing separator */
    if (memchr(rel, '\\', n) != NULL) return 0;            /* never a separator here */
    size_t start = 0, comps = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || rel[i] == '/') {
            /* An empty span here is a repeated separator or an empty
             * component; sr_cd_component_is_generic refuses len == 0. */
            if (!sr_cd_component_is_generic(rel + start, i - start)) return 0;
            comps++;
            start = i + 1;
        }
    }
    return comps > 0;
}

/* Split a canonical relative path into its parent path and its last component.
 * The last component is what gets destroyed; the parent is what anchors it. */
static inline int sr_cd_rel_split(const char *rel, char *parent, size_t pcap,
                                  char *last, size_t lcap) {
    if (!parent || !last || !sr_cd_rel_is_canonical(rel)) return 0;
    const char *slash = strrchr(rel, '/');
    size_t plen = slash ? (size_t)(slash - rel) : 0;
    const char *lstart = slash ? slash + 1 : rel;
    size_t llen = strlen(lstart);
    if (plen >= pcap || llen >= lcap) return 0;
    memcpy(parent, rel, plen);
    parent[plen] = '\0';
    memcpy(last, lstart, llen);
    last[llen] = '\0';
    return 1;
}

/* ---- Layer C hook: declared here, defined by each backend --------------- */
static inline int sr_cd_component_is_host_ok(const char *c, size_t len);

/* A caller-supplied NAME (a leaf to erase, or one component of rel): the
 * generic grammar and then this host's grammar. Both must pass. */
static inline int sr_cd_name_is_acceptable(const char *name, size_t len) {
    return sr_cd_component_is_generic(name, len) && sr_cd_component_is_host_ok(name, len);
}

/* A canonical relative path whose every component is also acceptable to this
 * host. Layer A first, so a structurally broken path is refused identically
 * everywhere; then Layer C. */
static inline int sr_cd_rel_is_acceptable(const char *rel) {
    if (!sr_cd_rel_is_canonical(rel)) return 0;
    size_t n = strlen(rel), start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || rel[i] == '/') {
            if (!sr_cd_component_is_host_ok(rel + start, i - start)) return 0;
            start = i + 1;
        }
    }
    return 1;
}

/* An ENUMERATED directory entry. These names came FROM the host, so the rule is
 * the same one applied to caller-supplied names: whatever this host can
 * legitimately store, this host's Layer C accepts, and it must stay removable
 * or a save directory holding one odd filename would be undeletable. Only what
 * could change the meaning of the removal -- "." , ".." , an embedded separator
 * -- is refused generically. */
static inline int sr_cd_entry_name_is_acceptable(const char *name, size_t len) {
    return sr_cd_name_is_acceptable(name, len);
}

/* ---- deterministic test seam (never compiled into production) ----------- */
/* SR_CD_TEST_HOOKS lets a selftest act at exactly the instant a hostile actor
 * would have to act, so the object-replacement cases below are decisions rather
 * than timing races. No production build input defines it; the selftest harness
 * supplies it only for deterministic fixture execution. */
enum {
    SR_CD_HOOK_AFTER_DRAIN = 0,   /* target emptied; its name not yet re-resolved */
    SR_CD_HOOK_AFTER_CONFIRM = 1, /* identity re-confirmed; removal not yet issued */
    SR_CD_HOOK_COUNT = 2
};

#if defined(SR_CD_TEST_HOOKS)
typedef void (*sr_cd_test_hook_fn)(void *ctx);
static sr_cd_test_hook_fn sr_cd__hook_fn[SR_CD_HOOK_COUNT];
static void *sr_cd__hook_ctx[SR_CD_HOOK_COUNT];

static inline void sr_cd_test_hook_set(int which, sr_cd_test_hook_fn fn, void *ctx) {
    if (which >= 0 && which < SR_CD_HOOK_COUNT) {
        sr_cd__hook_fn[which] = fn;
        sr_cd__hook_ctx[which] = ctx;
    }
}
static inline void sr_cd_test_hooks_clear(void) {
    for (int i = 0; i < SR_CD_HOOK_COUNT; i++) {
        sr_cd__hook_fn[i] = NULL;
        sr_cd__hook_ctx[i] = NULL;
    }
}
static inline void sr_cd__hook_fire(int which) {
    if (which >= 0 && which < SR_CD_HOOK_COUNT && sr_cd__hook_fn[which])
        sr_cd__hook_fn[which](sr_cd__hook_ctx[which]);
}
#else
#define sr_cd__hook_fire(which) ((void)(which))
#endif

/* ======================================================================== */
/* Windows backend: OPEN -> HANDLE -> FINAL PATH VERIFY -> OPERATION.       */
/* Every step delegates to the audited helpers in vfs_path.h, so this is a  */
/* re-expression of the shipped F114-1 behaviour behind the neutral seam,   */
/* not a new Windows implementation.                                        */
/* ======================================================================== */
#if defined(SR_CD_BACKEND_WINDOWS)

/* Layer C for Win32. Win32 path normalization resolves reserved device names in
 * the DOS namespace rather than the filesystem, strips trailing dots and
 * spaces, reads ':' as an ADS selector and '\' as a separator, and expands
 * wildcards -- each of those really would change which object is named.
 * sr_vfs_is_safe_component is the audited refusal for all of them. */
static inline int sr_cd_component_is_host_ok(const char *c, size_t len) {
    return sr_vfs_is_safe_component(c, len);
}

static inline int sr_cd__win_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (!sr_cd_name_is_acceptable(name, nl)) return 0;
    if (dl + 1u + nl + 1u > cap) return 0;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl + 1u);
    return 1;
}

static inline int sr_cd__win_dir_path(const sr_cd_root *root, const char *rel,
                                      char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s", root->prefix, rel);
    return n > 0 && (size_t)n < cap;
}

/* Binds the canonical identity of the root exactly once. Inherited documented
 * side effect (F114-4): sr_vfs_canonical_root creates the BARE root directory
 * if it is missing. See ROOT BINDING at sr_cd_delete_dir_shallow for why the
 * two backends may legitimately bind the root differently. */
static inline sr_cd_status sr_cd_root_open(const char *root_utf8, sr_cd_root *out) {
    if (!root_utf8 || !out) return SR_CD_NOT_CONTAINED;
    size_t rl = strlen(root_utf8);
    if (rl == 0 || rl >= sizeof(out->prefix)) return SR_CD_NOT_CONTAINED;
    memcpy(out->prefix, root_utf8, rl + 1u);
    if (!sr_vfs_canonical_root(root_utf8, out->canonical,
                               sizeof(out->canonical) / sizeof(out->canonical[0])))
        return SR_CD_NOT_CONTAINED;
    return SR_CD_OK;
}

static inline void sr_cd_root_close(sr_cd_root *root) { (void)root; }

static inline sr_cd_status sr_cd_delete_leaf(const sr_cd_root *root, const char *rel_dir,
                                             const char *leaf) {
    char dir[SR_CD_HOST_PATH_MAX], path[SR_CD_HOST_PATH_MAX];
    if (!root || !leaf) return SR_CD_NOT_CONTAINED;
    if (!sr_cd_rel_is_acceptable(rel_dir)) return SR_CD_INVALID_PATH;
    if (!sr_cd_name_is_acceptable(leaf, strlen(leaf))) return SR_CD_INVALID_PATH;
    if (!sr_cd__win_dir_path(root, rel_dir, dir, sizeof(dir))) return SR_CD_INVALID_PATH;
    if (!sr_cd__win_join(path, sizeof(path), dir, leaf)) return SR_CD_INVALID_PATH;
    int was_dir = 0;
    /* The disposition rides the handle whose final path was just verified, and
     * FILE_FLAG_OPEN_REPARSE_POINT pins it to the object named inside the root
     * rather than a link target. No by-name deletion happens after the check. */
    if (sr_vfs_delete_contained_leaf(path, root->canonical, &was_dir)) return SR_CD_OK;
    return was_dir ? SR_CD_IS_DIRECTORY : SR_CD_NOT_CONTAINED;
}

/* ROOT BINDING (documented once, applies to both backends).
 *
 * The two backends bind the operator-configured root differently and both are
 * defensible: Windows binds the canonical NAME, POSIX binds a DESCRIPTOR (an
 * inode). If the root's own directory is renamed away after binding, Windows
 * follows the configured path and POSIX follows the original object. Neither is
 * an escape -- in both cases every operation stays beneath a root the operator
 * pointed at -- so root containment is platform-defined by design.
 *
 * That is a SEPARATE contract from final-child object identity below. Do not
 * conflate them: root binding is about WHICH ROOT, object identity is about
 * WHICH CHILD within it.
 *
 * OBJECT IDENTITY on this backend is guaranteed. One verified handle is opened
 * BEFORE the drain and the disposition is set on that same handle afterwards,
 * so the object removed is exactly the object whose containment was verified.
 * The name is never resolved a second time and there is no window in which a
 * replacement could be substituted for it.
 *
 * Known bound, stated rather than claimed away: the per-ENTRY drain below is
 * name addressed, because Win32's public API has no descriptor-relative unlink.
 * Every entry is nonetheless re-opened and containment-verified individually
 * before its own handle carries the disposition, so a substituted entry is
 * still an object inside the operator's root -- containment holds, entry-level
 * object identity does not. */
static inline sr_cd_status sr_cd_delete_dir_shallow(const sr_cd_root *root, const char *rel) {
    char dir[SR_CD_HOST_PATH_MAX], path[SR_CD_HOST_PATH_MAX];
    if (!root) return SR_CD_NOT_CONTAINED;
    if (!sr_cd_rel_is_acceptable(rel)) return SR_CD_INVALID_PATH;
    if (!sr_cd__win_dir_path(root, rel, dir, sizeof(dir))) return SR_CD_INVALID_PATH;
    if (!sr_vfs_dir_is_contained(dir, root->canonical)) return SR_CD_NOT_CONTAINED;

    /* THE verified handle. Opened before anything is destroyed and held across
     * the whole operation; the disposition at the end rides it. */
    HANDLE hd;
    if (!sr_vfs_open_contained_utf8(dir, DELETE | FILE_READ_ATTRIBUTES,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    OPEN_EXISTING, root->canonical, &hd)) {
        return SR_CD_NOT_CONTAINED;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(hd, &info)) {
        CloseHandle(hd);
        return SR_CD_IO_ERROR;
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(hd);
        return SR_CD_NOT_CONTAINED;
    }
    /* A reparse point here would mean the handle names the LINK while the
     * enumeration below walks its TARGET -- two different objects. Refuse, so
     * "the object drained is the object disposed" holds unconditionally. */
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        CloseHandle(hd);
        return SR_CD_NOT_CONTAINED;
    }

    DIR *d = opendir(dir);
    if (!d) {
        CloseHandle(hd);
        return SR_CD_NOT_FOUND;
    }
    struct dirent *de;
    sr_cd_status entries = SR_CD_OK;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!sr_cd_entry_name_is_acceptable(de->d_name, strlen(de->d_name)) ||
            !sr_cd__win_join(path, sizeof(path), dir, de->d_name)) {
            entries = SR_CD_INVALID_PATH;
            continue;
        }
        int was_dir = 0;
        if (!sr_vfs_delete_contained_leaf(path, root->canonical, &was_dir) || was_dir) {
            entries = was_dir ? SR_CD_IS_DIRECTORY : SR_CD_NOT_CONTAINED;
        }
    }
    closedir(d);

    sr_cd__hook_fire(SR_CD_HOOK_AFTER_DRAIN);
    /* No re-confirmation step exists on this backend because no re-resolution
     * does: the identity was bound by the handle before the drain. The hook is
     * fired anyway so the replacement selftest exercises both backends at the
     * same two points. */
    sr_cd__hook_fire(SR_CD_HOOK_AFTER_CONFIRM);

    int removed = sr_vfs_dispose_by_handle(hd);
    CloseHandle(hd);
    if (entries != SR_CD_OK) return entries;
    return removed ? SR_CD_OK : SR_CD_NOT_EMPTY;
}

/* ======================================================================== */
/* POSIX backend: descriptor-relative traversal and deletion.               */
/* ======================================================================== */
#elif defined(SR_CD_BACKEND_POSIX_AT)

/* Layer C for POSIX: nothing. A POSIX filename may contain any byte except '/'
 * and NUL, both already refused by Layer A. Importing Win32's device names,
 * trailing-dot rule or wildcard set here would not add safety -- it would make
 * files this host can legitimately create impossible to delete, leaving a save
 * directory permanently stuck. */
static inline int sr_cd_component_is_host_ok(const char *c, size_t len) {
    (void)c;
    (void)len;
    return 1;
}

/* One component, opened relative to an ALREADY VERIFIED directory descriptor.
 * O_NOFOLLOW applies to the final component of the pathname handed to openat,
 * and the pathname here is exactly one component -- so every component of the
 * walk is a final component and none of them may be a symbolic link. An actor
 * who swaps an intermediate save-directory component for a link therefore gets
 * ELOOP, not a redirect. */
static inline int sr_cd__at_open_dir(int parent, const char *comp) {
    return openat(parent, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static inline sr_cd_status sr_cd__at_open_fail(void) {
    switch (errno) {
        case ENOENT: return SR_CD_NOT_FOUND;
        case ELOOP: return SR_CD_NOT_CONTAINED;
        case ENOTDIR: return SR_CD_NOT_CONTAINED;
        default: return SR_CD_IO_ERROR;
    }
}

/* Walk a canonical root-relative path to a directory descriptor. The root
 * descriptor is the anchor: it names an inode, so renaming or replacing the
 * root's PATH after binding cannot move the walk. Each subsequent component is
 * opened relative to the previous descriptor, so the namespace is never
 * re-entered from the top and no ancestor is ever re-resolved. An empty
 * relative path yields the root itself -- the caller has already applied the
 * canonical grammar, so the empty string reaches here only as the parent of a
 * single-component path. */
static inline sr_cd_status sr_cd__at_walk(int root_fd, const char *rel, int *out_fd) {
    int cur = dup(root_fd);
    if (cur < 0) return SR_CD_IO_ERROR;
    const char *p = rel ? rel : "";
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        char comp[SR_CD_NAME_MAX];
        if (len >= sizeof(comp) || !sr_cd_name_is_acceptable(start, len)) {
            close(cur);
            return SR_CD_INVALID_PATH;
        }
        memcpy(comp, start, len);
        comp[len] = '\0';
        int next = sr_cd__at_open_dir(cur, comp);
        if (next < 0) {
            sr_cd_status st = sr_cd__at_open_fail();
            close(cur);
            return st;
        }
        close(cur);
        cur = next;
        if (*p == '/') p++;
    }
    *out_fd = cur;
    return SR_CD_OK;
}

/* Classify a REFUSED unlinkat. This runs only after the kernel has already
 * declined to remove the entry, so it can never itself select a victim: it is
 * diagnosis, not a check whose result a later deletion trusts. That is the
 * whole point -- the directory/non-directory distinction is enforced by
 * unlinkat's own semantics (it cannot remove a directory without AT_REMOVEDIR),
 * not by a preceding stat whose answer an attacker could invalidate. */
static inline sr_cd_status sr_cd__at_unlink_fail(int dir_fd, const char *leaf) {
    int e = errno;
    if (e == ENOENT) return SR_CD_NOT_FOUND;
    if (e == EISDIR || e == EPERM) {
        struct stat st;
        if (fstatat(dir_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
            return SR_CD_IS_DIRECTORY;
    }
    return SR_CD_IO_ERROR;
}

static inline sr_cd_status sr_cd_root_open(const char *root_utf8, sr_cd_root *out) {
    if (!root_utf8 || !*root_utf8 || !out) return SR_CD_NOT_CONTAINED;
    out->fd = -1;
    /* The root is operator-configured and therefore trusted: its own path may
     * legitimately be a symlink, so this open resolves normally. Everything
     * BELOW it is guest-influenced and never resolved that way again. */
    int fd = open(root_utf8, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? SR_CD_NOT_FOUND : SR_CD_IO_ERROR;
    out->fd = fd;
    return SR_CD_OK;
}

static inline void sr_cd_root_close(sr_cd_root *root) {
    if (root && root->fd >= 0) {
        close(root->fd);
        root->fd = -1;
    }
}

static inline sr_cd_status sr_cd_delete_leaf(const sr_cd_root *root, const char *rel_dir,
                                             const char *leaf) {
    if (!root || root->fd < 0 || !leaf) return SR_CD_NOT_CONTAINED;
    if (!sr_cd_name_is_acceptable(leaf, strlen(leaf))) return SR_CD_INVALID_PATH;
    if (!sr_cd_rel_is_acceptable(rel_dir)) return SR_CD_INVALID_PATH;
    int dir_fd = -1;
    sr_cd_status st = sr_cd__at_walk(root->fd, rel_dir, &dir_fd);
    if (st != SR_CD_OK) return st;
    /* Descriptor-relative deletion. A leaf swapped between the walk and this
     * call still resolves inside THIS directory descriptor, so the blast radius
     * cannot leave the verified directory; if the replacement is a symlink,
     * unlinkat removes the link object and never touches its target. Leaf
     * deletion is therefore DIRECTORY bound, not object bound -- see the
     * guarantee table at the top of this header. */
    if (unlinkat(dir_fd, leaf, 0) != 0) st = sr_cd__at_unlink_fail(dir_fd, leaf);
    close(dir_fd);
    return st;
}

/* ROOT BINDING and OBJECT IDENTITY on this backend.
 *
 * Root binding: the root is bound as a DESCRIPTOR, so it follows the inode
 * rather than the configured name. Windows binds the canonical name instead.
 * Both are defensible and neither escapes an operator-configured root; root
 * containment is platform-defined by design. This is a SEPARATE contract from
 * the child-object identity discussed next.
 *
 * Object identity: NOT guaranteed, and deliberately not claimed.
 *
 * POSIX.1-2008 offers no way to remove a directory by descriptor and no atomic
 * compare-and-remove: unlinkat(AT_REMOVEDIR) and renameat are both name
 * addressed. Staging the target under a private name first does not help,
 * because the rename that would move it is itself name addressed. Exact
 * object-bound directory removal is therefore not expressible portably, and the
 * SR_CD_GUARANTEE_DIR_OBJECT_BOUND bit is correspondingly absent here.
 *
 * What IS implemented is detection with a fail-closed response. The target's
 * (st_dev, st_ino) is bound from the verified descriptor before the drain and
 * re-confirmed through a freshly opened descriptor immediately before removal.
 * If the name has stopped resolving to the bound object the operation returns
 * SR_CD_IDENTITY_CHANGED having removed NOTHING: a detected replacement is
 * never deleted.
 *
 * RESIDUAL WINDOW, stated exactly rather than papered over: a substitution
 * performed between the confirmation and the unlinkat is undetectable by
 * construction. Its blast radius is bounded on three sides -- the substitute
 * must live inside the bound root (the parent descriptor is verified), it must
 * be a directory (AT_REMOVEDIR), and it must be EMPTY (a non-empty directory
 * fails with ENOTEMPTY). So the worst case is removal of an empty in-root
 * directory that the actor deliberately placed at that exact name inside that
 * window. Closing this residual needs a primitive POSIX.1-2008 does not have; a
 * stronger backend may be added later behind a capability bit, and savedata
 * semantics will not change when it is. */
static inline sr_cd_status sr_cd_delete_dir_shallow(const sr_cd_root *root, const char *rel) {
    char parent_rel[SR_CD_REL_MAX], last[SR_CD_NAME_MAX];
    if (!root || root->fd < 0) return SR_CD_NOT_CONTAINED;
    if (!sr_cd_rel_is_acceptable(rel)) return SR_CD_INVALID_PATH;
    if (!sr_cd_rel_split(rel, parent_rel, sizeof(parent_rel), last, sizeof(last)))
        return SR_CD_INVALID_PATH;

    int parent_fd = -1;
    sr_cd_status st = sr_cd__at_walk(root->fd, parent_rel, &parent_fd);
    if (st != SR_CD_OK) return st;

    int dir_fd = sr_cd__at_open_dir(parent_fd, last);
    if (dir_fd < 0) {
        st = sr_cd__at_open_fail();
        close(parent_fd);
        return st;
    }
    /* Bind the identity of the object about to be emptied, from the verified
     * descriptor itself -- not from a name. */
    struct stat bound;
    if (fstat(dir_fd, &bound) != 0) {
        close(dir_fd);
        close(parent_fd);
        return SR_CD_IO_ERROR;
    }

    int saw_dir = 0, saw_err = 0;
    for (int pass = 0; pass < SR_CD_MAX_PASSES; pass++) {
        /* A FRESH open description per pass, anchored on dir_fd itself. dup()
         * would share the directory read offset with the previous pass, so the
         * second pass would resume mid-stream and a directory larger than one
         * batch would never fully drain. "." relative to dir_fd cannot be
         * redirected, so this stays anchored. */
        int enum_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (enum_fd < 0) { saw_err = 1; break; }
        DIR *d = fdopendir(enum_fd);
        if (!d) { close(enum_fd); saw_err = 1; break; }
        char names[SR_CD_BATCH][SR_CD_NAME_MAX];
        size_t n = 0;
        struct dirent *de;
        while (n < SR_CD_BATCH && (de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            size_t len = strlen(de->d_name);
            /* An entry this seam may not name is left in place; the
             * AT_REMOVEDIR below then fails closed rather than the tree
             * half-vanishing. */
            if (len >= SR_CD_NAME_MAX || !sr_cd_entry_name_is_acceptable(de->d_name, len)) {
                saw_err = 1;
                continue;
            }
            memcpy(names[n], de->d_name, len + 1u);
            n++;
        }
        closedir(d);
        if (n == 0) break;
        size_t progressed = 0;
        for (size_t i = 0; i < n; i++) {
            if (unlinkat(dir_fd, names[i], 0) == 0) { progressed++; continue; }
            sr_cd_status why = sr_cd__at_unlink_fail(dir_fd, names[i]);
            if (why == SR_CD_NOT_FOUND) { progressed++; continue; }
            if (why == SR_CD_IS_DIRECTORY) saw_dir = 1;
            saw_err = 1;
        }
        if (progressed == 0) break;
    }
    close(dir_fd);

    sr_cd__hook_fire(SR_CD_HOOK_AFTER_DRAIN);

    /* Re-confirm that `last` STILL names the object that was just emptied. A
     * replacement present at this point is detected here and nothing is
     * removed. This is not a type probe feeding a by-name delete: it compares
     * the identity of an OPEN descriptor against an identity bound from another
     * OPEN descriptor, and its only outcome on mismatch is refusal. */
    int confirm_fd = sr_cd__at_open_dir(parent_fd, last);
    if (confirm_fd < 0) {
        /* The name no longer opens as a directory at all: either it is gone, or
         * something else has taken its place. Either way, remove nothing. */
        st = sr_cd__at_open_fail();
        close(parent_fd);
        return st == SR_CD_NOT_FOUND ? SR_CD_NOT_FOUND : SR_CD_IDENTITY_CHANGED;
    }
    struct stat now;
    int same = fstat(confirm_fd, &now) == 0 && now.st_dev == bound.st_dev &&
               now.st_ino == bound.st_ino;
    close(confirm_fd);
    if (!same) {
        close(parent_fd);
        return SR_CD_IDENTITY_CHANGED;
    }

    sr_cd__hook_fire(SR_CD_HOOK_AFTER_CONFIRM);

    /* The save directory is removed relative to its VERIFIED parent descriptor,
     * so the removal cannot be redirected by a rename of any ancestor path. */
    sr_cd_status final_st = SR_CD_OK;
    if (unlinkat(parent_fd, last, AT_REMOVEDIR) != 0) {
        switch (errno) {
            case ENOENT: final_st = SR_CD_NOT_FOUND; break;
            case ENOTEMPTY:
#if defined(EEXIST)
            case EEXIST:
#endif
                final_st = SR_CD_NOT_EMPTY; break;
            case ENOTDIR: final_st = SR_CD_NOT_CONTAINED; break;
            default: final_st = SR_CD_IO_ERROR; break;
        }
    }
    close(parent_fd);

    if (saw_dir) return SR_CD_IS_DIRECTORY;
    if (final_st != SR_CD_OK) return final_st;
    return saw_err ? SR_CD_IO_ERROR : SR_CD_OK;
}

/* ======================================================================== */
/* Fail-closed backend for hosts with no containment implementation yet.    */
/* Deliberately destroys nothing rather than offering a pathname fallback.  */
/* ======================================================================== */
#else

/* No host grammar can be honoured because no host operation will be performed.
 * Refusing every name keeps the layering honest: this backend accepts nothing. */
static inline int sr_cd_component_is_host_ok(const char *c, size_t len) {
    (void)c;
    (void)len;
    return 0;
}

static inline sr_cd_status sr_cd_root_open(const char *root_utf8, sr_cd_root *out) {
    (void)root_utf8;
    if (out) out->unused = 0;
    return SR_CD_UNSUPPORTED_HOST;
}

static inline void sr_cd_root_close(sr_cd_root *root) { (void)root; }

static inline sr_cd_status sr_cd_delete_leaf(const sr_cd_root *root, const char *rel_dir,
                                             const char *leaf) {
    (void)root; (void)rel_dir; (void)leaf;
    return SR_CD_UNSUPPORTED_HOST;
}

static inline sr_cd_status sr_cd_delete_dir_shallow(const sr_cd_root *root, const char *rel) {
    (void)root; (void)rel;
    return SR_CD_UNSUPPORTED_HOST;
}

#endif

#endif /* SR_VFS_CONTAINED_H */
