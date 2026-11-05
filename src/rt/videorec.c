// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// videorec.c - video recording of a run on one guest timeline (#1273). See videorec.h.
//
// Pure C with no SDL or Vulkan dependency, so it builds in every runtime configuration and its
// selftest needs no GPU. The only host service beyond files is starting FFmpeg with a pipe on
// its standard input: CreateProcessW on Windows, posix_spawn on POSIX, never a shell.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "videorec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

/* ---- configuration and state ---------------------------------------------------------- */

enum { VR_UNREAD = 0, VR_OFF, VR_ON, VR_DONE };

#define VR_RING_FRAMES (1u << 20) /* about 23.8 s of mixed audio ahead of the flush point */
#define VR_PERIOD_US 16683u       /* one VBLANK period, rounded; only used to spot late VBLANKs */
/* Free space on the drive holding SR_VIDEOREC_DIR: a recording does not start with less than
 * VR_MIN_FREE_START, and stops (FAIL) when it falls under VR_MIN_FREE_RUN, checked every
 * VR_FREE_CHECK_FRAMES frames. A lossless 4K recording takes gigabytes a minute. */
#define VR_MIN_FREE_START ((uint64_t)4u << 30)
#define VR_MIN_FREE_RUN ((uint64_t)1u << 30)
#define VR_FREE_CHECK_FRAMES 60u

typedef struct {
#ifdef _WIN32
    HANDLE process;
    HANDLE in;
#else
    pid_t pid;
    int in;
#endif
    int running;
} VrProc;

static int s_state = VR_UNREAD;
static char s_dir[900];
static char s_codec[32];
static char s_ffmpeg[1024];
static char s_run_end[96];
/* The process exit status the route that ended the run named, or VR_EXIT_UNKNOWN. */
#define VR_EXIT_UNKNOWN (-0x7fffffff - 1)
static int s_exit_status = VR_EXIT_UNKNOWN;
static uint64_t s_enc_write_us; /* host time spent handing frames to the encoder */

/* Two packed BGRX pictures: the one last written (s_out) and the newest presented one, which a
 * present writes into the other buffer so a gap frame can still repeat the written picture. */
static uint8_t *s_buf[2];
static int s_out;
static uint8_t *s_cur;
static uint32_t s_w, s_h;
static int s_have_frame;
static int s_cur_new;
static SrVideoRecResolver s_resolver;

/* The timeline: video frame k is VBLANK s_vcount0 + k. */
static uint32_t s_start_vcount; /* SR_VIDEOREC_START: no frame is written before this VBLANK */
static uint32_t s_seen_vcount;  /* the newest VBLANK serviced, recorded or not */
static int s_seen_any;
static int s_started;
static uint32_t s_vcount0, s_last_vcount;
static uint64_t s_k;            /* index of the newest written frame */
static uint64_t s_tick_us;      /* guest time of the newest written frame's VBLANK */
static uint64_t s_frames, s_new_frames, s_repeat_frames, s_gap_frames, s_late_vblanks;
static uint64_t s_refused_frames, s_resolve_failures;

/* The mix: int32 stereo accumulators for samples [s_flushed, s_flushed + VR_RING_FRAMES). */
static int32_t *s_ring;
static uint64_t s_flushed;
static uint64_t s_ch_pos[SR_VIDEOREC_CHANNELS];
static int s_ch_seen[SR_VIDEOREC_CHANNELS];
static uint64_t s_audio_samples, s_audio_overflow, s_audio_trimmed, s_audio_before_start;
static uint64_t s_audio_clipped, s_max_lead;

/* The guest's own frame rate, kept apart from the picture: the flips each written frame
 * carries (frames.csv) and one subtitle cue per 60 frames (logic.srt), so the footage needs no
 * host readout drawn into it and still says honestly how many game frames each second had. */
static uint64_t s_flips_pending, s_guest_flips;
static uint64_t s_block_flips;
static FILE *s_srt;
static unsigned s_srt_cues;

static FILE *s_wav;
static FILE *s_csv;
static FILE *s_raw;
static VrProc s_enc;
static int s_failed;
static char s_fail_why[256];

static void vr_fail(const char *why) {
    if (!s_failed) {
        snprintf(s_fail_why, sizeof s_fail_why, "%s", why);
        fprintf(stderr, "VIDEOREC: FAIL %s\n", why);
        fflush(stderr);
    }
    s_failed = 1;
}

uint64_t sr_videorec_frame_sample(uint64_t k) {
    /* 44100 * 1001 / 60000 samples per frame, exactly: 44144100 / 60000. */
    return (k * 44144100u) / 60000u;
}

static void vr_path(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "%s/%s", s_dir, name);
}

/* The same file as an FFmpeg "file:" URL, so a folder that starts with '-' or contains "proto:"
 * is never read as an option or a protocol. */
static void vr_url(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "file:%s/%s", s_dir, name);
}

static uint64_t vr_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)freq.QuadPart, t = (uint64_t)c.QuadPart;
    return t / f * 1000000u + t % f * 1000000u / f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

#ifdef SR_VIDEOREC_SELFTEST
static int s_test_free_set;
static uint64_t s_test_free;
static int s_test_audio_write_fails;
static const char *s_test_clear_fail_at; /* an output name whose move aside fails */
#endif

/* Free bytes on the drive holding s_dir. Returns 0 when it cannot be read. */
static int vr_free_bytes(uint64_t *out) {
#ifdef SR_VIDEOREC_SELFTEST
    if (s_test_free_set) {
        *out = s_test_free;
        return 1;
    }
#endif
#ifdef _WIN32
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExA(s_dir, &avail, NULL, NULL)) return 0;
    *out = (uint64_t)avail.QuadPart;
#else
    struct statvfs v;
    if (statvfs(s_dir, &v) != 0) return 0;
    *out = (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
#endif
    return 1;
}

/* A JSON string, escaped. */
static int vr_json_str(FILE *f, const char *s) {
    if (fputc('"', f) == EOF) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int r;
        if (*p == '"' || *p == '\\') r = fprintf(f, "\\%c", *p);
        else if (*p < 0x20) r = fprintf(f, "\\u%04x", *p);
        else r = fputc(*p, f) == EOF ? -1 : 1;
        if (r < 0) return 0;
    }
    return fputc('"', f) != EOF;
}

/* Replace record.json with what `fill` writes, through record.json.tmp, so a reader never sees a
 * half-written summary. Returns 0 and says so on stderr when it could not be written. */
static int vr_replace_record(int (*fill)(FILE *, const void *), const void *ctx) {
    char tmp[1024], p[1024];
    vr_path(tmp, sizeof tmp, "record.json.tmp");
    vr_path(p, sizeof p, "record.json");
    FILE *j = fopen(tmp, "w");
    int ok = j != NULL && fill(j, ctx);
    if (j && (ferror(j) | fclose(j))) ok = 0;
#ifdef _WIN32
    if (ok && !MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING)) ok = 0;
#else
    if (ok && rename(tmp, p) != 0) ok = 0;
#endif
    if (!ok) {
        remove(tmp);
        fprintf(stderr, "VIDEOREC: FAIL record.json could not be written in SR_VIDEOREC_DIR\n");
        fflush(stderr);
    }
    return ok;
}

typedef struct {
    const char *status;
    const char *reason;
} VrShortRecord;

static int vr_fill_short(FILE *j, const void *ctx) {
    const VrShortRecord *r = (const VrShortRecord *)ctx;
    return fprintf(j, "{\n  \"schema\": 2,\n  \"status\": \"%s\",\n  \"reason\": ", r->status) >= 0 &&
           vr_json_str(j, r->reason) && fprintf(j, "\n}\n") >= 0;
}

static int vr_write_short_record(const char *status, const char *reason) {
    VrShortRecord r = { status, reason };
    return vr_replace_record(vr_fill_short, &r);
}

/* The files a recording writes in s_dir, which a new start clears. */
static const char *const k_vr_outputs[] = { "record.json", "record.json.tmp", "recording.mkv",
                                            "video.mkv", "video.bgr0", "audio.wav", "frames.csv",
                                            "logic.srt", NULL };
#define VR_ASIDE_SUFFIX ".videorec-old"

/* Bytes the earlier run's output files hold: the clear frees them, so the start's free-space
 * check counts them as free. */
static uint64_t vr_outputs_bytes(void) {
    uint64_t total = 0;
    for (int i = 0; k_vr_outputs[i]; i++) {
        char p[1024];
        vr_path(p, sizeof p, k_vr_outputs[i]);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
    }
    return total;
}

/* Remove what an earlier run left in s_dir, so nothing there can pass for this run's output, all
 * or nothing: every file is first renamed aside (NAME.videorec-old), and only when all of them
 * moved are the moved files deleted. A file that cannot be moved (open in another program, or a
 * folder where a file belongs) puts back the ones already moved and returns 0, so a start that
 * cannot clear the folder leaves the earlier recording whole. A leftover aside file from an earlier
 * clear is removed first; one that cannot be deleted at the end stays, named as aside, and is
 * never read as output. */
static int vr_clear_outputs(void) {
    int moved = 0;
    for (int i = 0; k_vr_outputs[i]; i++) {
        char p[1024], aside[1100];
        vr_path(p, sizeof p, k_vr_outputs[i]);
        snprintf(aside, sizeof aside, "%s" VR_ASIDE_SUFFIX, p);
        (void)remove(aside);
        struct stat st;
        int ok = 1;
        if (stat(p, &st) == 0) {
            ok = S_ISREG(st.st_mode) && rename(p, aside) == 0;
#ifdef SR_VIDEOREC_SELFTEST
            if (ok && s_test_clear_fail_at && strcmp(s_test_clear_fail_at, k_vr_outputs[i]) == 0) {
                (void)rename(aside, p);
                ok = 0;
            }
#endif
        } else {
            continue;
        }
        if (!ok) {
            /* Put back what moved, in reverse. */
            for (int j = i - 1; j >= 0; j--) {
                char q[1024], qa[1100];
                vr_path(q, sizeof q, k_vr_outputs[j]);
                snprintf(qa, sizeof qa, "%s" VR_ASIDE_SUFFIX, q);
                if (stat(qa, &st) == 0 && rename(qa, q) != 0) {
                    fprintf(stderr, "VIDEOREC: %s could not be put back from %s" VR_ASIDE_SUFFIX
                                    " in SR_VIDEOREC_DIR\n", k_vr_outputs[j], k_vr_outputs[j]);
                    fflush(stderr);
                }
            }
            return 0;
        }
        moved++;
    }
    for (int i = 0; moved && k_vr_outputs[i]; i++) {
        char p[1024], aside[1100];
        vr_path(p, sizeof p, k_vr_outputs[i]);
        snprintf(aside, sizeof aside, "%s" VR_ASIDE_SUFFIX, p);
        (void)remove(aside);
    }
    return 1;
}

/* A recording that cannot start: say why on stderr and in record.json, record nothing. */
static void vr_refuse(const char *why) {
    fprintf(stderr, "VIDEOREC: not recording: %s\n", why);
    fflush(stderr);
    (void)vr_write_short_record("REFUSED", why);
}

/* ---- FFmpeg ---------------------------------------------------------------------------- */

#ifdef _WIN32
static int vr_file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int vr_is_absolute(const char *p) {
    return (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
           (p[0] == '\\' && p[1] == '\\');
}
#define VR_PATH_SEP ';'
#define VR_EXE_NAME "ffmpeg.exe"
#else
static int vr_file_exists(const char *path) { return access(path, X_OK) == 0; }
static int vr_is_absolute(const char *p) { return p[0] == '/'; }
#define VR_PATH_SEP ':'
#define VR_EXE_NAME "ffmpeg"
#endif

/* FFmpeg from SR_VIDEOREC_FFMPEG (an absolute path) or the first absolute PATH entry that holds
 * it. Relative PATH entries, the working directory and the runtime's own folder are never
 * searched, so a file dropped beside a game cannot stand in for the encoder. A refusal names the
 * key, never the path: logs and record.json get pasted into issues, and a path can hold a user
 * name. */
static int vr_find_ffmpeg(char *out, size_t outsz, char *why, size_t whysz) {
    const char *forced = getenv("SR_VIDEOREC_FFMPEG");
    if (forced && forced[0]) {
        if (!vr_is_absolute(forced)) {
            snprintf(why, whysz,
                     "the custom FFmpeg path in SR_VIDEOREC_FFMPEG is not an absolute path");
            return 0;
        }
        if (!vr_file_exists(forced) || strlen(forced) >= outsz) {
            snprintf(why, whysz,
                     "the custom FFmpeg path in SR_VIDEOREC_FFMPEG is not an executable file");
            return 0;
        }
        snprintf(out, outsz, "%s", forced);
        return 1;
    }
    snprintf(why, whysz, "%s is not in any absolute PATH entry; install FFmpeg, set "
                         "SR_VIDEOREC_FFMPEG to its absolute path, or set SR_VIDEOREC_CODEC=raw",
             VR_EXE_NAME);
    const char *path = getenv("PATH");
    if (!path) return 0;
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, VR_PATH_SEP);
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > 0 && len < 900) {
            char dir[1024];
            memcpy(dir, p, len);
            dir[len] = '\0';
            if (dir[0] == '"' && len > 1 && dir[len - 1] == '"') {
                memmove(dir, dir + 1, len - 2);
                dir[len - 2] = '\0';
            }
            if (vr_is_absolute(dir)) {
                int w = snprintf(out, outsz, "%s/%s", dir, VR_EXE_NAME);
                if (w > 0 && (size_t)w < outsz && vr_file_exists(out)) {
                    why[0] = '\0';
                    return 1;
                }
                out[0] = 0;
            }
        }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

#ifdef _WIN32
/* One argument quoted for CommandLineToArgvW / the C runtime's parser. */
static int vr_append_arg(char *cmd, size_t cap, const char *arg) {
    size_t n = strlen(cmd);
    if (n && n + 1 < cap) cmd[n++] = ' ';
    if (n + 1 >= cap) return 0;
    cmd[n++] = '"';
    for (const char *a = arg;; a++) {
        size_t slashes = 0;
        while (*a == '\\') { slashes++; a++; }
        size_t need = !*a ? slashes * 2 : (*a == '"' ? slashes * 2 + 2 : slashes + 1);
        if (n + need + 2 >= cap) return 0;
        if (!*a) {
            for (size_t i = 0; i < slashes * 2; i++) cmd[n++] = '\\';
            break;
        }
        if (*a == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) cmd[n++] = '\\';
            cmd[n++] = '"';
        } else {
            for (size_t i = 0; i < slashes; i++) cmd[n++] = '\\';
            cmd[n++] = *a;
        }
    }
    cmd[n++] = '"';
    cmd[n] = '\0';
    return 1;
}

static int vr_proc_start(VrProc *p, const char *exe, const char *const *argv, int with_stdin) {
    memset(p, 0, sizeof *p);
    static char cmd[8192];
    static wchar_t wcmd[8192];
    static wchar_t wexe[1024];
    cmd[0] = '\0';
    for (int i = 0; argv[i]; i++)
        if (!vr_append_arg(cmd, sizeof cmd, argv[i])) return 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, (int)(sizeof wcmd / sizeof wcmd[0])) ||
        !MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, (int)(sizeof wexe / sizeof wexe[0])))
        return 0;
    HANDLE rd = NULL, wr = NULL;
    if (with_stdin) {
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        /* The pipe holds about two frames (1 to 64 MiB): the encoder may lag by a frame without
         * the writer waiting, and one that falls further behind makes the writes wait, which
         * encoder_write_ms counts. On the deterministic clock that wait costs wall-clock time only.
         * A larger pipe would only hide a slow encoder until it filled. */
        uint64_t two_frames = (uint64_t)s_w * s_h * 4u * 2u;
        DWORD pipe_bytes = two_frames < (1u << 20)    ? (DWORD)(1u << 20)
                           : two_frames > (64u << 20) ? (DWORD)(64u << 20)
                                                      : (DWORD)two_frames;
        if (!CreatePipe(&rd, &wr, &sa, pipe_bytes)) return 0;
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = with_stdin ? rd : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessW(wexe, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (rd) CloseHandle(rd);
    if (!ok) {
        if (wr) CloseHandle(wr);
        return 0;
    }
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->in = wr;
    p->running = 1;
    return 1;
}

static int vr_proc_write(VrProc *p, const void *buf, size_t len) {
    const uint8_t *b = (const uint8_t *)buf;
    while (len) {
        DWORD chunk = len > (1u << 30) ? (1u << 30) : (DWORD)len, done = 0;
        if (!WriteFile(p->in, b, chunk, &done, NULL) || done == 0) return 0;
        b += done;
        len -= done;
    }
    return 1;
}

/* Close the encoder's input and wait for it. Returns its exit status, or -1. */
static int vr_proc_finish(VrProc *p) {
    if (!p->running) return -1;
    if (p->in) CloseHandle(p->in);
    p->in = NULL;
    DWORD code = 1;
    if (WaitForSingleObject(p->process, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(p->process, &code))
        code = (DWORD)-1;
    CloseHandle(p->process);
    p->running = 0;
    return (int)code;
}
#else
static int vr_proc_start(VrProc *p, const char *exe, const char *const *argv, int with_stdin) {
    memset(p, 0, sizeof *p);
    p->in = -1;
    int fds[2] = { -1, -1 };
    if (with_stdin && pipe(fds) != 0) return 0;
    /* posix_spawn, not fork: the runtime has many threads, and the child must run no
     * runtime code between fork and exec. The encoder reads frames on stdin and writes its
     * console output to the runtime's stderr. A program that cannot start fails here. */
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        if (with_stdin) { close(fds[0]); close(fds[1]); }
        return 0;
    }
    int ok = 1;
    if (with_stdin) {
        ok = posix_spawn_file_actions_adddup2(&fa, fds[0], 0) == 0 &&
             posix_spawn_file_actions_addclose(&fa, fds[0]) == 0 &&
             posix_spawn_file_actions_addclose(&fa, fds[1]) == 0;
    }
    ok = ok && posix_spawn_file_actions_adddup2(&fa, 2, 1) == 0;
    pid_t pid = -1;
    if (ok) ok = posix_spawn(&pid, exe, &fa, NULL, (char *const *)argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    if (!ok) {
        if (with_stdin) { close(fds[0]); close(fds[1]); }
        return 0;
    }
    if (with_stdin) {
        close(fds[0]);
        p->in = fds[1];
        /* A dead encoder must surface as a failed write, not end the runtime. */
        signal(SIGPIPE, SIG_IGN);
    }
    p->pid = pid;
    p->running = 1;
    return 1;
}

static int vr_proc_write(VrProc *p, const void *buf, size_t len) {
    const uint8_t *b = (const uint8_t *)buf;
    while (len) {
        ssize_t n = write(p->in, b, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        b += n;
        len -= (size_t)n;
    }
    return 1;
}

static int vr_proc_finish(VrProc *p) {
    if (!p->running) return -1;
    if (p->in >= 0) close(p->in);
    p->in = -1;
    int status = 0;
    pid_t r;
    do { r = waitpid(p->pid, &status, 0); } while (r < 0 && errno == EINTR);
    p->running = 0;
    if (r < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
#endif

/* The encoder settings for SR_VIDEOREC_CODEC. Every one is lossless for the 8-bit BGR input: NVENC
 * needs -pix_fmt gbrp for that, since from BGR it otherwise converts to 4:2:0 YUV first (measured:
 * decoded RGB differs without it and matches with it, as it does for ffv1 and x264rgb). */
static int vr_codec_args(const char *codec, const char **out, int max) {
    static const char *ffv1[] = { "-c:v", "ffv1", "-level", "3", "-g", "1", "-slices", "24",
                                  "-slicecrc", "1", "-threads", "0", NULL };
    static const char *x264rgb[] = { "-c:v", "libx264rgb", "-preset", "ultrafast", "-qp", "0",
                                     NULL };
    static const char *nvenc[] = { "-c:v", "hevc_nvenc", "-preset", "p1", "-tune", "lossless",
                                   "-pix_fmt", "gbrp", NULL };
    const char **src = NULL;
    if (strcmp(codec, "ffv1") == 0) src = ffv1;
    else if (strcmp(codec, "x264rgb") == 0) src = x264rgb;
    else if (strcmp(codec, "nvenc") == 0) src = nvenc;
    if (!src) return -1;
    int n = 0;
    while (src[n] && n < max - 1) { out[n] = src[n]; n++; }
    out[n] = NULL;
    return n;
}

static int vr_start_encoder(void) {
    char size[32], video[1024];
    snprintf(size, sizeof size, "%ux%u", (unsigned)s_w, (unsigned)s_h);
    vr_url(video, sizeof video, "video.mkv");
    const char *argv[48];
    int n = 0;
    argv[n++] = "ffmpeg";
    argv[n++] = "-hide_banner";
    argv[n++] = "-loglevel";
    argv[n++] = "error";
    argv[n++] = "-y";
    argv[n++] = "-f";
    argv[n++] = "rawvideo";
    argv[n++] = "-pix_fmt";
    argv[n++] = "bgr0";
    argv[n++] = "-video_size";
    argv[n++] = size;
    argv[n++] = "-framerate";
    argv[n++] = "60000/1001";
    argv[n++] = "-i";
    argv[n++] = "-";
    int c = vr_codec_args(s_codec, argv + n, 24);
    if (c < 0) return 0;
    n += c;
    argv[n++] = video;
    argv[n] = NULL;
    return vr_proc_start(&s_enc, s_ffmpeg, argv, 1);
}

/* ---- files ----------------------------------------------------------------------------- */

static void vr_put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void vr_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void vr_wav_header(uint8_t h[44], uint64_t samples) {
    uint64_t bytes64 = samples * 4u;
    uint32_t bytes = bytes64 > 0xFFFFFFD3u ? 0xFFFFFFD3u : (uint32_t)bytes64;
    memcpy(h, "RIFF", 4);
    vr_put_le32(h + 4, 36u + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    vr_put_le32(h + 16, 16u);
    vr_put_le16(h + 20, 1u);
    vr_put_le16(h + 22, 2u);
    vr_put_le32(h + 24, SR_VIDEOREC_AUDIO_RATE);
    vr_put_le32(h + 28, SR_VIDEOREC_AUDIO_RATE * 4u);
    vr_put_le16(h + 32, 4u);
    vr_put_le16(h + 34, 16u);
    memcpy(h + 36, "data", 4);
    vr_put_le32(h + 40, bytes);
}

static int vr_make_dir(const char *dir) {
#ifdef _WIN32
    if (_mkdir(dir) == 0 || errno == EEXIST) return 1;
#else
    if (mkdir(dir, 0755) == 0 || errno == EEXIST) return 1;
#endif
    return 0;
}

static int vr_open_files(void) {
    char p[1024];
    vr_path(p, sizeof p, "audio.wav");
    s_wav = fopen(p, "wb");
    vr_path(p, sizeof p, "frames.csv");
    s_csv = fopen(p, "w");
    vr_path(p, sizeof p, "logic.srt");
    s_srt = fopen(p, "w");
    if (!s_wav || !s_csv || !s_srt) return 0;
    uint8_t h[44];
    vr_wav_header(h, 0);
    if (fwrite(h, 1, sizeof h, s_wav) != sizeof h) return 0;
    if (fputs("frame,vcount,guest_us,kind,guest_flips\n", s_csv) < 0) return 0;
    s_ring = (int32_t *)calloc((size_t)VR_RING_FRAMES * 2u, sizeof *s_ring);
    return s_ring != NULL;
}

/* Whether guest time follows guest work. The runtime's main() hands the recorder the
 * scheduler's answer (sr_videorec_set_clock(sched_clock_deterministic)); the scheduler parses
 * SR_DETERMINISTIC_CLOCK once (pace_setup, which also rejects any value but 0 and 1 and the
 * SR_NOVBPACE=0 contradiction), so the recorder and the clock cannot disagree. The recorder links
 * with the presenter (SDL3VK_SRCS), and programs that link the presenter without a scheduler
 * (the GPU selftests, ge-replay) set no clock: there the recorder refuses, saying why. The
 * standalone selftest links no scheduler and reads the key the same way. */
static int (*s_clock_deterministic)(void);

void sr_videorec_set_clock(int (*deterministic)(void)) { s_clock_deterministic = deterministic; }

#ifdef SR_VIDEOREC_SELFTEST
static int vr_clock_deterministic(void) {
    const char *det = getenv("SR_DETERMINISTIC_CLOCK");
    return det && strcmp(det, "1") == 0;
}
#else
static int vr_clock_deterministic(void) {
    return s_clock_deterministic ? s_clock_deterministic() : 0;
}
#endif

/* Start a recording into dir, or refuse with the reason in record.json and on stderr.
 * need_det_clock: the runtime records only on the deterministic clock (videorec.h). */
static int vr_begin(const char *dir, const char *codec, int need_det_clock) {
    if (!dir || !dir[0] || strlen(dir) >= sizeof s_dir - 32) {
        fprintf(stderr, "VIDEOREC: not recording: SR_VIDEOREC_DIR is empty or too long\n");
        return 0;
    }
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    if (!vr_make_dir(s_dir)) {
        fprintf(stderr, "VIDEOREC: not recording: SR_VIDEOREC_DIR cannot be created\n");
        return 0;
    }
    /* Every refusal check comes before the folder is cleared: a refused start replaces only
     * record.json (REFUSED, so no earlier PASS stands) and leaves the earlier run's recording. */
    char why[1200];
    if (need_det_clock) {
#ifndef SR_VIDEOREC_SELFTEST
        if (!s_clock_deterministic) {
            vr_refuse("this program runs no scheduler, so it has no deterministic guest clock "
                      "to record on");
            return 0;
        }
#endif
        if (!vr_clock_deterministic()) {
            vr_refuse("SR_VIDEOREC_DIR needs SR_DETERMINISTIC_CLOCK=1. On the paced clock the "
                      "frame readback, copy and encoder writes hold the guest thread, so the "
                      "guest would skip VBLANKs a run without the recording keeps");
            return 0;
        }
    }
    snprintf(s_codec, sizeof s_codec, "%s", codec && codec[0] ? codec : "ffv1");
    const char *probe[24];
    if (strcmp(s_codec, "raw") != 0 && vr_codec_args(s_codec, probe, 24) < 0) {
        snprintf(why, sizeof why, "SR_VIDEOREC_CODEC '%s' is not ffv1, x264rgb, nvenc or raw",
                 s_codec);
        vr_refuse(why);
        return 0;
    }
    char ffwhy[1100];
    if (strcmp(s_codec, "raw") != 0 && !vr_find_ffmpeg(s_ffmpeg, sizeof s_ffmpeg, ffwhy, sizeof ffwhy)) {
        snprintf(why, sizeof why, "encoder %s needs FFmpeg: %s", s_codec, ffwhy);
        vr_refuse(why);
        return 0;
    }
    uint64_t free_bytes = 0;
    if (!vr_free_bytes(&free_bytes)) {
        vr_refuse("the free space on SR_VIDEOREC_DIR's drive cannot be read");
        return 0;
    }
    /* The clear below frees the earlier run's files, so they count as free here: checking after
     * the clear would delete the earlier recording before a refusal. */
    free_bytes += vr_outputs_bytes();
    if (free_bytes < VR_MIN_FREE_START) {
        snprintf(why, sizeof why, "SR_VIDEOREC_DIR's drive has %llu MiB free; a recording needs "
                                  "at least %llu MiB",
                 (unsigned long long)(free_bytes >> 20),
                 (unsigned long long)(VR_MIN_FREE_START >> 20));
        vr_refuse(why);
        return 0;
    }
    /* This start records: remove what an earlier run left, so nothing there can pass for this
     * run's output. A file that cannot be removed refuses the start with every earlier file in
     * place, as REFUSED when record.json can be replaced. When record.json itself is the file that
     * cannot be replaced (open in another program), no record can say so: stderr does, and the
     * earlier record stays. */
    if (!vr_clear_outputs()) {
        vr_refuse("an earlier run's files in SR_VIDEOREC_DIR cannot be removed (one is open in "
                  "another program, or is a folder); none was removed");
        return 0;
    }
    /* Until the finish rewrites it, the summary says the recording is not finished. */
    if (!vr_write_short_record("INCOMPLETE", "the run ended before the recording was finished")) {
        fprintf(stderr, "VIDEOREC: not recording: record.json cannot be written\n");
        return 0;
    }
    if (!vr_open_files()) {
        FILE **open_files[] = { &s_wav, &s_csv, &s_srt };
        for (size_t i = 0; i < sizeof open_files / sizeof open_files[0]; i++)
            if (*open_files[i]) { fclose(*open_files[i]); *open_files[i] = NULL; }
        vr_refuse("the recording files cannot be created in SR_VIDEOREC_DIR");
        return 0;
    }
    fprintf(stderr, "VIDEOREC: recording every VBLANK at 60000/1001 fps, audio 44100 Hz, "
                    "encoder %s\n", s_codec);
    fflush(stderr);
    return 1;
}

static void vr_finish_at_exit(void) { (void)sr_videorec_finish(); }

static void vr_init_once(void) {
    if (s_state != VR_UNREAD) return;
    s_state = VR_OFF;
    const char *dir = getenv("SR_VIDEOREC_DIR");
    if (!dir || !dir[0]) return;
    if (!vr_begin(dir, getenv("SR_VIDEOREC_CODEC"), 1)) return;
    const char *start = getenv("SR_VIDEOREC_START");
    unsigned long sv = start && start[0] ? strtoul(start, NULL, 10) : 0ul;
    s_start_vcount = sv > 0xFFFFFFFFul ? 0xFFFFFFFFu : (uint32_t)sv;
    s_state = VR_ON;
    atexit(vr_finish_at_exit);
}

int sr_videorec_active(void) {
    vr_init_once();
    return s_state == VR_ON;
}

int sr_videorec_wants_frames(void) {
    if (!sr_videorec_active()) return 0;
    if (s_started || !s_start_vcount) return 1;
    /* The picture shown at the start VBLANK is presented during the period before it. */
    return s_seen_any && s_seen_vcount + 1u >= s_start_vcount;
}

int sr_videorec_requested_size(uint32_t *w, uint32_t *h) {
    const char *v = getenv("SR_VIDEOREC_SIZE");
    if (!v || !v[0]) return 0;
    char *end = NULL;
    unsigned long rw = strtoul(v, &end, 10);
    if (!end || (*end != 'x' && *end != 'X')) return 0;
    unsigned long rh = strtoul(end + 1, &end, 10);
    if (!end || *end || rw < 16 || rh < 16 || rw > 16384 || rh > 16384 || (rw & 1u) || (rh & 1u))
        return 0;
    *w = (uint32_t)rw;
    *h = (uint32_t)rh;
    return 1;
}

/* ---- frames ---------------------------------------------------------------------------- */

void sr_videorec_frame(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                       sr_cap_order order) {
    if (!sr_videorec_wants_frames() || !px || !w || !h || row_bytes < w * 4u) return;
    if (!s_buf[0]) {
        if ((uint64_t)w * h * 4u > (uint64_t)1 << 31) { vr_fail("frame too large"); return; }
        s_buf[0] = (uint8_t *)calloc((size_t)w * h, 4u);
        s_buf[1] = (uint8_t *)calloc((size_t)w * h, 4u);
        if (!s_buf[0] || !s_buf[1]) { vr_fail("out of memory for the frame store"); return; }
        s_w = w;
        s_h = h;
        s_out = 1;
    } else if (w != s_w || h != s_h) {
        if (s_refused_frames++ == 0)
            fprintf(stderr, "VIDEOREC: a %ux%u frame in a %ux%u recording was not written; the "
                            "previous picture stands in for it\n", w, h, s_w, s_h);
        return;
    }
    const uint8_t *src = (const uint8_t *)px;
    s_cur = s_buf[1 - s_out];
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * row_bytes;
        uint8_t *d = s_cur + (size_t)y * w * 4u;
        if (order == SR_CAP_ORDER_BGRX) {
            memcpy(d, s, (size_t)w * 4u);
        } else {
            for (uint32_t x = 0; x < w; x++) {
                d[x * 4u + 0] = s[x * 4u + 2];
                d[x * 4u + 1] = s[x * 4u + 1];
                d[x * 4u + 2] = s[x * 4u + 0];
                d[x * 4u + 3] = 0;
            }
        }
    }
    s_have_frame = 1;
    s_cur_new = 1;
}

void sr_videorec_frame_pending(SrVideoRecResolver resolver) {
    if (!sr_videorec_active()) return;
    s_resolver = resolver;
}

void sr_videorec_resolve_pending(void) {
    SrVideoRecResolver r = s_resolver;
    if (!r) return;
    s_resolver = NULL;
    if (!r()) {
        s_resolve_failures++;
        vr_fail("a presented frame could not be read back");
    }
}

static void vr_srt_time(char *out, size_t n, uint64_t frame) {
    uint64_t ms = frame * 1001000u / 60000u;
    snprintf(out, n, "%02llu:%02llu:%02llu,%03llu", (unsigned long long)(ms / 3600000u),
             (unsigned long long)(ms / 60000u % 60u), (unsigned long long)(ms / 1000u % 60u),
             (unsigned long long)(ms % 1000u));
}

/* One logic.srt cue for frames [from, to): the guest flips in that stretch, per second. */
static void vr_srt_cue(uint64_t from, uint64_t to) {
    if (!s_srt || to <= from) return;
    char a[32], b[32];
    vr_srt_time(a, sizeof a, from);
    vr_srt_time(b, sizeof b, to);
    double fps = (double)s_block_flips * 60000.0 / (1001.0 * (double)(to - from));
    if (fprintf(s_srt, "%u\n%s --> %s\ngame %.1f fps (%llu frames)\n\n", ++s_srt_cues, a, b, fps,
                (unsigned long long)s_block_flips) < 0)
        vr_fail("logic.srt write failed");
    s_block_flips = 0;
}

/* Stop writing video after a failure that makes more of it pointless (the disk filling up). */
static void vr_stop_video(void) {
    if (s_enc.running) (void)vr_proc_finish(&s_enc);
    if (s_raw) {
        fclose(s_raw);
        s_raw = NULL;
    }
}

static void vr_check_free_space(void) {
    uint64_t free_bytes = 0;
    if (!vr_free_bytes(&free_bytes)) {
        vr_fail("the free space on SR_VIDEOREC_DIR's drive could not be read; recording stopped");
        vr_stop_video();
    } else if (free_bytes < VR_MIN_FREE_RUN) {
        vr_fail("SR_VIDEOREC_DIR's drive has under 1 GiB free; recording stopped");
        vr_stop_video();
    }
}

void sr_videorec_guest_flip(void) {
    if (sr_videorec_wants_frames()) s_flips_pending++;
}

static void vr_write_frame(uint32_t vcount, uint64_t guest_us, const char *kind) {
    if (s_failed && !s_raw && !s_enc.running) return;
    size_t bytes = (size_t)s_w * s_h * 4u;
    const uint8_t *px = s_buf[s_out];
    if (s_enc.running) {
        uint64_t t0 = vr_now_us();
        int ok = vr_proc_write(&s_enc, px, bytes);
        s_enc_write_us += vr_now_us() - t0;
        if (!ok) {
            vr_fail("the encoder stopped taking frames");
            (void)vr_proc_finish(&s_enc);
        }
    } else if (s_raw) {
        if (fwrite(px, 1, bytes, s_raw) != bytes) vr_fail("raw video write failed");
    }
    uint64_t flips = 0;
    if (strcmp(kind, "gap") != 0) {
        flips = s_flips_pending;
        s_flips_pending = 0;
    }
    if (fprintf(s_csv, "%llu,%u,%llu,%s,%llu\n", (unsigned long long)s_frames, vcount,
                (unsigned long long)guest_us, kind, (unsigned long long)flips) < 0)
        vr_fail("frames.csv write failed");
    s_guest_flips += flips;
    s_block_flips += flips;
    s_frames++;
    if (s_frames % 60u == 0u) vr_srt_cue(s_frames - 60u, s_frames);
    if (s_frames % VR_FREE_CHECK_FRAMES == 0u) vr_check_free_space();
}

/* Finalise mixed samples [s_flushed, upto) into the WAV file. */
static void vr_flush_audio(uint64_t upto) {
    int16_t buf[2048 * 2];
    while (s_flushed < upto) {
        uint64_t left = upto - s_flushed;
        size_t n = left > 2048u ? 2048u : (size_t)left;
        for (size_t i = 0; i < n; i++) {
            size_t r = (size_t)((s_flushed + i) & (VR_RING_FRAMES - 1u)) * 2u;
            for (int c = 0; c < 2; c++) {
                int32_t v = s_ring[r + (size_t)c];
                if (v > 32767) { v = 32767; s_audio_clipped++; }
                if (v < -32768) { v = -32768; s_audio_clipped++; }
                buf[i * 2u + (size_t)c] = (int16_t)v;
                s_ring[r + (size_t)c] = 0;
            }
        }
        uint8_t le[2048 * 4];
        for (size_t i = 0; i < n * 2u; i++) vr_put_le16(le + i * 2u, (uint16_t)buf[i]);
        /* Only samples that reached audio.wav count: the finish compares that count with the
         * video's length, so a failed write shows there. */
        size_t written = s_wav ? fwrite(le, 4u, n, s_wav) : 0u;
#ifdef SR_VIDEOREC_SELFTEST
        if (s_test_audio_write_fails) written = 0;
#endif
        s_flushed += n;
        s_audio_samples += written;
    }
}

void sr_videorec_vblank(uint32_t vcount, uint64_t guest_us) {
    if (!sr_videorec_active()) return;
    sr_videorec_resolve_pending();
    s_seen_vcount = vcount;
    s_seen_any = 1;
    if (!s_started) {
        if (vcount < s_start_vcount) return;
        if (!s_have_frame) return;  /* the recording starts at the first presented picture */
        s_started = 1;
        s_vcount0 = vcount;
        if (strcmp(s_codec, "raw") == 0) {
            char p[1024];
            vr_path(p, sizeof p, "video.bgr0");
            s_raw = fopen(p, "wb");
            if (!s_raw) vr_fail("cannot create video.bgr0");
        } else if (!vr_start_encoder()) {
            vr_fail("FFmpeg did not start");
        }
        fprintf(stderr, "VIDEOREC: started at vblank %u, %ux%u\n", vcount, s_w, s_h);
        fflush(stderr);
        s_k = 0;
        s_tick_us = guest_us;
        s_last_vcount = vcount;
        s_out = 1 - s_out;
        vr_write_frame(vcount, guest_us, "new");
        s_new_frames++;
        s_cur_new = 0;
        return;
    }
    if (vcount == s_last_vcount) return;  /* already written for this VBLANK */
    /* VCOUNT steps no tick was serviced for keep their place on the timeline as gap frames. */
    uint32_t steps = vcount - s_last_vcount;
    for (uint32_t i = 1; i < steps; i++) {
        s_k++;
        vr_flush_audio(sr_videorec_frame_sample(s_k));
        vr_write_frame(s_last_vcount + i, guest_us, "gap");
        s_gap_frames++;
    }
    if (guest_us - s_tick_us < VR_PERIOD_US / 2u) s_late_vblanks++;
    s_k++;
    vr_flush_audio(sr_videorec_frame_sample(s_k));
    if (s_cur_new) {
        s_out = 1 - s_out;
        vr_write_frame(vcount, guest_us, "new");
        s_new_frames++;
    } else {
        vr_write_frame(vcount, guest_us, "repeat");
        s_repeat_frames++;
    }
    s_cur_new = 0;
    s_last_vcount = vcount;
    s_tick_us = guest_us;
}

/* ---- audio ----------------------------------------------------------------------------- */

void sr_videorec_audio(int ch, const int16_t *lr, int nframes, int volL, int volR,
                       uint64_t guest_us) {
    if (s_state != VR_ON || !lr || nframes <= 0 || ch < 0 || ch >= SR_VIDEOREC_CHANNELS) return;
    if (!s_started) {
        s_audio_before_start += (uint64_t)nframes;
        return;
    }
    /* Where this guest instant falls on the audio timeline: the start of the current frame plus
     * the guest time since its VBLANK, never past the next frame's start. */
    uint64_t base = sr_videorec_frame_sample(s_k);
    uint64_t span = sr_videorec_frame_sample(s_k + 1) - base;
    uint64_t into = guest_us > s_tick_us
                        ? (guest_us - s_tick_us) * SR_VIDEOREC_AUDIO_RATE / 1000000u : 0u;
    uint64_t now = base + (into < span ? into : span);
    /* A channel plays its buffers back to back; one that ran dry restarts now. */
    uint64_t pos = s_ch_seen[ch] && s_ch_pos[ch] > now ? s_ch_pos[ch] : now;
    s_ch_seen[ch] = 1;
    for (int i = 0; i < nframes; i++) {
        uint64_t at = pos + (uint64_t)i;
        if (at >= s_flushed + VR_RING_FRAMES) {
            s_audio_overflow += (uint64_t)(nframes - i);
            break;
        }
        size_t r = (size_t)(at & (VR_RING_FRAMES - 1u)) * 2u;
        s_ring[r] += (int32_t)(((int64_t)lr[i * 2] * volL) >> 15);
        s_ring[r + 1u] += (int32_t)(((int64_t)lr[i * 2 + 1] * volR) >> 15);
    }
    s_ch_pos[ch] = pos + (uint64_t)nframes;
    if (s_ch_pos[ch] - now > s_max_lead) s_max_lead = s_ch_pos[ch] - now;
}

/* ---- finish ---------------------------------------------------------------------------- */

static int vr_mux(void) {
    char video[1024], audio[1024], out[1024], srt[1024];
    vr_url(video, sizeof video, "video.mkv");
    vr_url(audio, sizeof audio, "audio.wav");
    vr_url(out, sizeof out, "recording.mkv");
    vr_url(srt, sizeof srt, "logic.srt");
    /* The logic-rate subtitles go in as a track that is off by default. */
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                           "-i", video, "-i", audio, "-i", srt,
                           "-map", "0:v:0", "-map", "1:a:0", "-map", "2:s:0",
                           "-c", "copy", "-metadata:s:s:0", "title=Game frame rate",
                           "-disposition:s:0", "0", out, NULL };
    VrProc p;
    if (!vr_proc_start(&p, s_ffmpeg, argv, 0)) return -1;
    return vr_proc_finish(&p);
}

typedef struct {
    int enc_status, mux_status;
    uint64_t end;
} VrFinal;

static int vr_fill_full(FILE *j, const void *ctx) {
    const VrFinal *f = (const VrFinal *)ctx;
    return fprintf(j, "{\n  \"schema\": 2,\n  \"status\": \"%s\",\n  \"reason\": ",
                   s_failed ? "FAIL" : "PASS") >= 0 &&
           vr_json_str(j, s_failed ? s_fail_why : "") &&
           fprintf(j, ",\n  \"run_end\": ") >= 0 && vr_json_str(j, s_run_end) &&
           (s_exit_status == VR_EXIT_UNKNOWN ? fprintf(j, ",\n  \"exit_status\": null")
                                             : fprintf(j, ",\n  \"exit_status\": %d", s_exit_status)) >= 0 &&
           fprintf(j,
                   ",\n  \"clock\": \"deterministic\",\n"
                   "  \"encoder\": \"%s\",\n"
                   "  \"video\": \"%s\",\n  \"muxed\": %s,\n"
                   "  \"width\": %u,\n  \"height\": %u,\n"
                   "  \"fps_num\": %u,\n  \"fps_den\": %u,\n  \"first_vcount\": %u,\n"
                   "  \"frames\": %llu,\n  \"new_frames\": %llu,\n  \"repeat_frames\": %llu,\n"
                   "  \"guest_flips\": %llu,\n"
                   "  \"gap_frames\": %llu,\n  \"late_vblanks\": %llu,\n  \"refused_frames\": %llu,\n"
                   "  \"readback_failures\": %llu,\n"
                   "  \"audio_rate\": %u,\n  \"audio_samples\": %llu,\n"
                   "  \"audio_samples_expected\": %llu,\n  \"audio_before_start\": %llu,\n"
                   "  \"audio_trimmed_at_end\": %llu,\n  \"audio_overflow\": %llu,\n"
                   "  \"audio_clipped\": %llu,\n  \"audio_max_channel_lead\": %llu,\n"
                   "  \"encoder_write_ms\": %llu,\n"
                   "  \"encoder_exit\": %d,\n  \"mux_exit\": %d\n}\n",
                   s_codec, strcmp(s_codec, "raw") == 0 ? "video.bgr0" : "video.mkv",
                   /* raw has no mux step: null, so no PASS ever says muxed: false */
                   strcmp(s_codec, "raw") == 0 ? "null" : f->mux_status == 0 ? "true" : "false",
                   s_w, s_h, SR_VIDEOREC_FPS_NUM,
                   SR_VIDEOREC_FPS_DEN, s_vcount0, (unsigned long long)s_frames,
                   (unsigned long long)s_new_frames, (unsigned long long)s_repeat_frames,
                   (unsigned long long)s_guest_flips,
                   (unsigned long long)s_gap_frames, (unsigned long long)s_late_vblanks,
                   (unsigned long long)s_refused_frames, (unsigned long long)s_resolve_failures,
                   SR_VIDEOREC_AUDIO_RATE, (unsigned long long)s_audio_samples,
                   (unsigned long long)f->end, (unsigned long long)s_audio_before_start,
                   (unsigned long long)s_audio_trimmed, (unsigned long long)s_audio_overflow,
                   (unsigned long long)s_audio_clipped, (unsigned long long)s_max_lead,
                   (unsigned long long)(s_enc_write_us / 1000u), f->enc_status,
                   f->mux_status) >= 0;
}

static void vr_close_checked(FILE **f, const char *why) {
    if (!*f) return;
    if (ferror(*f) | fclose(*f)) vr_fail(why);
    *f = NULL;
}

/* The atexit fallback: an exit() call no route named, such as one of the runtime's exit(1) or
 * exit(2) fatal paths. The recorder cannot see that call's status, so record.json says so
 * ("exit_status": null) rather than calling the run a clean one. */
#define VR_RUN_END_UNNAMED "an exit() that names no end; its exit status is unknown"

int sr_videorec_finish(void) { return sr_videorec_finish_at(VR_RUN_END_UNNAMED); }

int sr_videorec_finish_at(const char *run_end) {
    return sr_videorec_finish_status(run_end, VR_EXIT_UNKNOWN);
}

int sr_videorec_finish_status(const char *run_end, int exit_status) {
    if (s_state != VR_ON) return s_state == VR_DONE ? !s_failed : 0;
    s_state = VR_DONE;
    snprintf(s_run_end, sizeof s_run_end, "%s", run_end ? run_end : "");
    s_exit_status = exit_status;
    uint64_t end = s_started ? sr_videorec_frame_sample(s_frames) : 0u;
    vr_flush_audio(end);
    for (int c = 0; c < SR_VIDEOREC_CHANNELS; c++)
        if (s_ch_seen[c] && s_ch_pos[c] > end) s_audio_trimmed += s_ch_pos[c] - end;
    if (s_wav) {
        uint8_t h[44];
        vr_wav_header(h, s_audio_samples);
        if (fseek(s_wav, 0, SEEK_SET) != 0 || fwrite(h, 1, sizeof h, s_wav) != sizeof h)
            vr_fail("audio header write failed");
        vr_close_checked(&s_wav, "audio.wav could not be closed");
    }
    vr_close_checked(&s_csv, "frames.csv could not be written");
    if (s_srt) {
        if (s_frames % 60u) vr_srt_cue(s_frames - s_frames % 60u, s_frames);
        vr_close_checked(&s_srt, "logic.srt could not be written");
    }
    vr_close_checked(&s_raw, "video.bgr0 could not be written");
    VrFinal fin = { -2, -2, end };
    if (s_enc.running) {
        fin.enc_status = vr_proc_finish(&s_enc);
        if (fin.enc_status != 0) vr_fail("the encoder exited with an error");
        else if (s_frames) fin.mux_status = vr_mux();
    }
    /* An encoded recording passes only once FFmpeg muxed it and exited 0: any other exit,
     * including a Windows exception code (negative as an int), is a failure. */
    if (strcmp(s_codec, "raw") != 0 && s_started && fin.mux_status != 0)
        vr_fail(fin.mux_status == -2 ? "the recording was not muxed"
                                     : "muxing audio and video failed");
    if (!s_started) vr_fail("no frame was presented while recording");
    if (s_refused_frames) vr_fail("frames of another size were presented");
    if (s_audio_overflow) vr_fail("audio ran more than 23 s ahead of the picture");
    if (s_started && s_audio_samples != end) vr_fail("audio.wav is shorter than the video");

    int written = vr_replace_record(vr_fill_full, &fin);
    char exit_text[16] = "unknown";
    if (s_exit_status != VR_EXIT_UNKNOWN) snprintf(exit_text, sizeof exit_text, "%d", s_exit_status);
    fprintf(stderr,
            "VIDEOREC: %s frames=%llu new=%llu repeat=%llu gap=%llu late_vblanks=%llu "
            "audio_samples=%llu encoder=%s encoder_write_ms=%llu run_end=\"%s\" exit_status=%s%s%s\n",
            s_failed || !written ? "FAIL" : "PASS", (unsigned long long)s_frames,
            (unsigned long long)s_new_frames, (unsigned long long)s_repeat_frames,
            (unsigned long long)s_gap_frames, (unsigned long long)s_late_vblanks,
            (unsigned long long)s_audio_samples, s_codec,
            (unsigned long long)(s_enc_write_us / 1000u), s_run_end, exit_text,
            s_failed ? " reason=" : "", s_failed ? s_fail_why : "");
    fflush(stderr);
    return !s_failed && written;
}

#ifdef SR_VIDEOREC_SELFTEST
int sr_videorec_test_start(const char *dir, const char *codec) {
    return sr_videorec_test_start_at(dir, codec, 0u);
}

int sr_videorec_test_start_at(const char *dir, const char *codec, uint32_t start_vcount) {
    sr_videorec_test_reset();
    s_state = VR_OFF;
    if (!vr_begin(dir, codec, 0)) return 0;
    s_start_vcount = start_vcount;
    s_state = VR_ON;
    return 1;
}

int sr_videorec_test_start_from_env(void) {
    sr_videorec_test_reset();
    return sr_videorec_active();
}

void sr_videorec_test_set_free_bytes(uint64_t bytes) {
    s_test_free_set = 1;
    s_test_free = bytes;
}

void sr_videorec_test_fail_audio_writes(int on) { s_test_audio_write_fails = on; }

void sr_videorec_test_fail_clear_at(const char *name) { s_test_clear_fail_at = name; }

void sr_videorec_test_stats(SrVideoRecStats *out) {
    memset(out, 0, sizeof *out);
    out->frames = s_frames;
    out->new_frames = s_new_frames;
    out->guest_flips = s_guest_flips;
    out->repeat_frames = s_repeat_frames;
    out->gap_frames = s_gap_frames;
    out->late_vblanks = s_late_vblanks;
    out->refused_frames = s_refused_frames;
    out->audio_samples = s_audio_samples;
    out->audio_overflow = s_audio_overflow;
    out->audio_trimmed = s_audio_trimmed;
    out->width = s_w;
    out->height = s_h;
    out->first_vcount = s_vcount0;
    out->failed = s_failed;
}

void sr_videorec_test_reset(void) {
    if (s_wav) fclose(s_wav);
    if (s_csv) fclose(s_csv);
    if (s_srt) fclose(s_srt);
    if (s_raw) fclose(s_raw);
    if (s_enc.running) (void)vr_proc_finish(&s_enc);
    free(s_buf[0]);
    free(s_buf[1]);
    s_buf[0] = s_buf[1] = NULL;
    s_out = 0;
    free(s_ring);
    s_state = VR_UNREAD;
    s_dir[0] = s_codec[0] = s_ffmpeg[0] = s_fail_why[0] = s_run_end[0] = '\0';
    s_exit_status = VR_EXIT_UNKNOWN;
    s_enc_write_us = 0;
    /* The selftest does not depend on the host disk: plenty of space unless a test says not. */
    s_test_free_set = 1;
    s_test_free = (uint64_t)1u << 40;
    s_test_audio_write_fails = 0;
    s_test_clear_fail_at = NULL;
    s_cur = NULL;
    s_ring = NULL;
    s_wav = s_csv = s_raw = s_srt = NULL;
    s_flips_pending = s_guest_flips = s_block_flips = 0;
    s_srt_cues = 0;
    memset(&s_enc, 0, sizeof s_enc);
    s_w = s_h = 0;
    s_have_frame = s_cur_new = s_started = s_failed = 0;
    s_resolver = NULL;
    s_vcount0 = s_last_vcount = 0;
    s_start_vcount = s_seen_vcount = 0;
    s_seen_any = 0;
    s_k = s_tick_us = 0;
    s_frames = s_new_frames = s_repeat_frames = s_gap_frames = s_late_vblanks = 0;
    s_refused_frames = s_resolve_failures = 0;
    s_flushed = 0;
    memset(s_ch_pos, 0, sizeof s_ch_pos);
    memset(s_ch_seen, 0, sizeof s_ch_seen);
    s_audio_samples = s_audio_overflow = s_audio_trimmed = s_audio_before_start = 0;
    s_audio_clipped = s_max_lead = 0;
}
#endif
