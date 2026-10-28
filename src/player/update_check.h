/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Check for updates (owner decision 2026-10-28, AGENTS.md section 5).
 *
 * The player asks GitHub's public Releases API which Nakagawa versions exist,
 * only when the user presses Check for updates or has turned on the check at
 * start. The request is anonymous: no ID, no hardware or game information.
 * Downloading the newer Windows ZIP is a second, separate click; the ZIP is
 * checked against the release's published SHA-256 (and GitHub's own digest of
 * the asset when the API lists one) and deleted on any mismatch. Install only
 * unpacks the verified ZIP into a new folder; it never replaces the running
 * version and never touches the user's data, which lives in the per-user data
 * folders, not in the install folder.
 *
 * Every network call goes through a PackageHttpOpen transport (package_builder.h):
 * NULL selects the native WinHTTP one, and tests pass their own, so no test
 * touches the internet. Nothing here starts a thread; the player runs these
 * calls on its own worker. */

#ifndef NAKAGAWA_UPDATE_CHECK_H
#define NAKAGAWA_UPDATE_CHECK_H

#include "package_builder.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one repository and the only hosts the update check reaches (the "Network
   use" table in docs/SETUP.md; tools/test_prereq_fetcher.py holds the list). */
#define NK_UPDATE_REPOSITORY "Jstar269/nakagawa-recomp"
#define NK_UPDATE_API_HOSTS "api.github.com"
#define NK_UPDATE_RELEASES_URL \
    "https://api.github.com/repos/" NK_UPDATE_REPOSITORY "/releases?per_page=10"
/* Release assets download from github.com, which redirects to GitHub's asset host. */
#define NK_UPDATE_DOWNLOAD_HOSTS "github.com;release-assets.githubusercontent.com"
#define NK_UPDATE_RELEASE_PAGE_PREFIX "https://github.com/" NK_UPDATE_REPOSITORY "/releases/tag/"
#define NK_UPDATE_DOWNLOAD_PREFIX "https://github.com/" NK_UPDATE_REPOSITORY "/releases/download/"
#define NK_UPDATE_USER_AGENT "NakagawaRecomp/UpdateCheck"

/* The largest answers read into memory: the release list and a checksum file. */
#define NK_UPDATE_MAX_RELEASES_BYTES (4u * 1024u * 1024u)
#define NK_UPDATE_MAX_CHECKSUM_BYTES 4096u

/* A Semantic Versioning 2.0.0 version. A tag may start with "v"; build
   metadata (+...) is accepted and ignored, as the standard says. */
typedef struct {
    unsigned long major;
    unsigned long minor;
    unsigned long patch;
    char prerelease[64];   /* "" for a release; "dev", "rc.1" */
} NkSemver;

bool nk_semver_parse(const char *text, NkSemver *out);
/* <0, 0 or >0 as a has lower, equal or higher precedence than b. */
int nk_semver_compare(const NkSemver *a, const NkSemver *b);

typedef struct {
    char code[48];
    char title[96];
    char message[512];
} NkUpdateError;

typedef struct {
    char tag[64];             /* "v0.0.2" */
    char version[64];         /* "0.0.2" */
    NkSemver semver;
    bool prerelease;
    char page_url[256];       /* the release page on github.com */
    char published[16];       /* "2026-10-28", or "" */
    char notes[480];          /* the first lines of the release notes, as plain text */
    bool has_zip;             /* the release lists the Windows ZIP */
    char zip_name[128];       /* "nakagawa-recomp-0.0.2-windows-x64.zip" */
    char zip_url[512];
    uint64_t zip_size;
    char zip_digest[65];      /* GitHub's SHA-256 of the ZIP, or "" when not listed */
    bool has_checksum;        /* the release lists "<zip>.sha256" */
    char checksum_url[512];
    uint64_t checksum_size;
} NkUpdateRelease;

typedef struct {
    NkUpdateRelease newest;
    NkSemver current;
    bool newer;               /* newest is newer than the running version */
} NkUpdateCheckResult;

/* Picks the newest release in a GitHub Releases API answer (a JSON array).
   Drafts and tags that are not versions are skipped; pre-releases count. Fails
   closed when a release names a page or an asset outside this repository. */
bool nk_update_parse_releases(const char *json, size_t length, NkUpdateRelease *newest,
                              NkUpdateError *error);

/* Release notes (Markdown) to a few plain sentences: quotes, headings and
   formatting are dropped, links keep their text. */
void nk_update_short_notes(const char *markdown, char *out, size_t out_size);

/* Reads a "<sha256>  <file name>" checksum file for file_name. */
bool nk_update_parse_checksum(const char *text, size_t length, const char *file_name,
                              char out_sha256[65], NkUpdateError *error);

/* The SHA-256 the ZIP must have: the release's checksum file, which must agree
   with GitHub's own digest of the ZIP when the API lists one. A release with
   no checksum file is refused (CHECKSUM_MISSING). */
bool nk_update_expected_sha256(const NkUpdateRelease *release, const char *checksum_text,
                               size_t checksum_length, char out_sha256[65],
                               NkUpdateError *error);

/* Asks GitHub for the release list and compares the newest with current_version. */
bool nk_update_check(PackageHttpOpen transport, void *transport_context,
                     const char *current_version, NkUpdateCheckResult *result,
                     NkUpdateError *error);

/* Downloads the release's Windows ZIP into downloads_dir and verifies it. On
   success out_zip_path names the verified file; on any failure nothing is left
   behind. progress may cancel by returning false. */
bool nk_update_download(PackageHttpOpen transport, void *transport_context,
                        const NkUpdateRelease *release, const char *downloads_dir,
                        PackageDownloadContinue progress, void *progress_context,
                        char *out_zip_path, size_t out_zip_path_size, NkUpdateError *error);

/* The folder Install unpacks zip_name into: beside the running version's folder
   when the player runs from a release ZIP's bin folder (exe_dir), otherwise in
   downloads_dir. The folder is named after the ZIP without ".zip". */
bool nk_update_install_folder(const char *exe_dir, const char *downloads_dir,
                              const char *zip_name, char *out, size_t out_size);

/* Unpacks the verified ZIP into target_dir, which must not exist yet: first into
   "<target_dir>.partial", then checks that every file the release's SHA256SUMS.txt
   lists arrived, then renames it into place. log_path takes the unpacker's
   output. Windows only (Windows' own tar.exe unpacks). */
bool nk_update_install(const char *zip_path, const char *target_dir, const char *log_path,
                       NkUpdateError *error);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_UPDATE_CHECK_H */
