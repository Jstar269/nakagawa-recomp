// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// videorec.c - video recording of a run on one guest timeline (#1273). See videorec.h.
//
// Pure C with no SDL or Vulkan dependency, so it builds in every runtime configuration and its
// selftest needs no GPU. The only host service beyond files is starting FFmpeg with a pipe on
// its standard input: CreateProcessW on Windows, fork/execv on POSIX, never a shell.
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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---- configuration and state ---------------------------------------------------------- */

enum { VR_UNREAD = 0, VR_OFF, VR_ON, VR_DONE };

#define VR_RING_FRAMES (1u << 20) /* about 23.8 s of mixed audio ahead of the flush point */
#define VR_PERIOD_US 16683u       /* one VBLANK period, rounded; only used to spot late VBLANKs */

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
static char s_encoder_note[160];
static char s_ffmpeg[1024];

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
 * searched, so a file dropped beside a game cannot stand in for the encoder. */
static int vr_find_ffmpeg(char *out, size_t outsz) {
    const char *forced = getenv("SR_VIDEOREC_FFMPEG");
    if (forced && forced[0]) {
        if (!vr_is_absolute(forced) || !vr_file_exists(forced)) return 0;
        snprintf(out, outsz, "%s", forced);
        return 1;
    }
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
                if (w > 0 && (size_t)w < outsz && vr_file_exists(out)) return 1;
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
        /* A 64 MiB pipe lets the encoder lag by a frame or two without stalling the runtime. */
        if (!CreatePipe(&rd, &wr, &sa, 64u << 20)) return 0;
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
    pid_t pid = fork();
    if (pid < 0) {
        if (with_stdin) { close(fds[0]); close(fds[1]); }
        return 0;
    }
    if (pid == 0) {
        if (with_stdin) {
            dup2(fds[0], 0);
            close(fds[0]);
            close(fds[1]);
        }
        dup2(2, 1);
        execv(exe, (char *const *)argv);
        _exit(127);
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

/* The encoder settings for SR_VIDEOREC_CODEC. Every one is lossless for the 8-bit BGR input. */
static int vr_codec_args(const char *codec, const char **out, int max) {
    static const char *ffv1[] = { "-c:v", "ffv1", "-level", "3", "-g", "1", "-slices", "24",
                                  "-slicecrc", "1", "-threads", "0", NULL };
    static const char *x264rgb[] = { "-c:v", "libx264rgb", "-preset", "ultrafast", "-qp", "0",
                                     NULL };
    static const char *nvenc[] = { "-c:v", "hevc_nvenc", "-preset", "p1", "-tune", "lossless",
                                   NULL };
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
    vr_path(video, sizeof video, "video.mkv");
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
    if (!vr_make_dir(s_dir)) return 0;
    vr_path(p, sizeof p, "audio.wav");
    s_wav = fopen(p, "wb");
    vr_path(p, sizeof p, "frames.csv");
    s_csv = fopen(p, "w");
    if (!s_wav || !s_csv) return 0;
    uint8_t h[44];
    vr_wav_header(h, 0);
    if (fwrite(h, 1, sizeof h, s_wav) != sizeof h) return 0;
    fputs("frame,vcount,guest_us,kind\n", s_csv);
    s_ring = (int32_t *)calloc((size_t)VR_RING_FRAMES * 2u, sizeof *s_ring);
    return s_ring != NULL;
}

static int vr_begin(const char *dir, const char *codec) {
    if (!dir || !dir[0] || strlen(dir) >= sizeof s_dir - 32) {
        fprintf(stderr, "VIDEOREC: SR_VIDEOREC_DIR is empty or too long; not recording\n");
        return 0;
    }
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    snprintf(s_codec, sizeof s_codec, "%s", codec && codec[0] ? codec : "ffv1");
    const char *probe[24];
    if (strcmp(s_codec, "raw") != 0 && vr_codec_args(s_codec, probe, 24) < 0) {
        fprintf(stderr, "VIDEOREC: SR_VIDEOREC_CODEC '%s' is not ffv1, x264rgb, nvenc or raw; "
                        "not recording\n", s_codec);
        return 0;
    }
    if (strcmp(s_codec, "raw") != 0 && !vr_find_ffmpeg(s_ffmpeg, sizeof s_ffmpeg)) {
        snprintf(s_encoder_note, sizeof s_encoder_note,
                 "FFmpeg not found on PATH; raw frames written instead of %s", s_codec);
        fprintf(stderr, "VIDEOREC: %s\n", s_encoder_note);
        snprintf(s_codec, sizeof s_codec, "raw");
    }
    if (!vr_open_files()) {
        fprintf(stderr, "VIDEOREC: cannot create the recording files in SR_VIDEOREC_DIR; "
                        "not recording\n");
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
    if (!vr_begin(dir, getenv("SR_VIDEOREC_CODEC"))) return;
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

static void vr_write_frame(uint32_t vcount, uint64_t guest_us, const char *kind) {
    if (s_failed && !s_raw && !s_enc.running) return;
    size_t bytes = (size_t)s_w * s_h * 4u;
    const uint8_t *px = s_buf[s_out];
    if (s_enc.running) {
        if (!vr_proc_write(&s_enc, px, bytes)) {
            vr_fail("the encoder stopped taking frames");
            (void)vr_proc_finish(&s_enc);
        }
    } else if (s_raw) {
        if (fwrite(px, 1, bytes, s_raw) != bytes) vr_fail("raw video write failed");
    }
    fprintf(s_csv, "%llu,%u,%llu,%s\n", (unsigned long long)s_frames, vcount,
            (unsigned long long)guest_us, kind);
    s_frames++;
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
        if (s_wav && fwrite(le, 1, n * 4u, s_wav) != n * 4u) vr_fail("audio write failed");
        s_flushed += n;
        s_audio_samples += n;
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
    /* VCOUNT steps no tick serviced (the host fell behind) keep their place on the timeline. */
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
    char video[1024], audio[1024], out[1024];
    vr_path(video, sizeof video, "video.mkv");
    vr_path(audio, sizeof audio, "audio.wav");
    vr_path(out, sizeof out, "recording.mkv");
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                           "-i", video, "-i", audio, "-map", "0:v:0", "-map", "1:a:0",
                           "-c", "copy", out, NULL };
    VrProc p;
    if (!vr_proc_start(&p, s_ffmpeg, argv, 0)) return -1;
    return vr_proc_finish(&p);
}

int sr_videorec_finish(void) {
    if (s_state != VR_ON) return s_state == VR_DONE ? !s_failed : 0;
    s_state = VR_DONE;
    uint64_t end = s_started ? sr_videorec_frame_sample(s_frames) : 0u;
    vr_flush_audio(end);
    for (int c = 0; c < SR_VIDEOREC_CHANNELS; c++)
        if (s_ch_seen[c] && s_ch_pos[c] > end) s_audio_trimmed += s_ch_pos[c] - end;
    if (s_wav) {
        uint8_t h[44];
        vr_wav_header(h, s_audio_samples);
        if (fseek(s_wav, 0, SEEK_SET) != 0 || fwrite(h, 1, sizeof h, s_wav) != sizeof h)
            vr_fail("audio header write failed");
        if (fclose(s_wav) != 0) vr_fail("audio close failed");
        s_wav = NULL;
    }
    if (s_csv) { fclose(s_csv); s_csv = NULL; }
    if (s_raw) {
        if (fclose(s_raw) != 0) vr_fail("raw video close failed");
        s_raw = NULL;
    }
    int enc_status = -2, mux_status = -2;
    if (s_enc.running) {
        enc_status = vr_proc_finish(&s_enc);
        if (enc_status != 0) vr_fail("the encoder exited with an error");
        else if (s_frames) mux_status = vr_mux();
        if (mux_status > 0 || mux_status == -1) vr_fail("muxing audio and video failed");
    }
    if (!s_started) vr_fail("no frame was presented while recording");
    if (s_refused_frames) vr_fail("frames of another size were presented");
    if (s_audio_overflow) vr_fail("audio ran more than 23 s ahead of the picture");
    if (s_started && s_audio_samples != end) vr_fail("audio length differs from the video's");

    char p[1024];
    vr_path(p, sizeof p, "record.json");
    FILE *j = fopen(p, "w");
    if (j) {
        fprintf(j,
                "{\n  \"schema\": 1,\n  \"status\": \"%s\",\n  \"reason\": \"%s\",\n"
                "  \"encoder\": \"%s\",\n  \"encoder_note\": \"%s\",\n"
                "  \"video\": \"%s\",\n  \"muxed\": %s,\n"
                "  \"width\": %u,\n  \"height\": %u,\n"
                "  \"fps_num\": %u,\n  \"fps_den\": %u,\n  \"first_vcount\": %u,\n"
                "  \"frames\": %llu,\n  \"new_frames\": %llu,\n  \"repeat_frames\": %llu,\n"
                "  \"gap_frames\": %llu,\n  \"late_vblanks\": %llu,\n  \"refused_frames\": %llu,\n"
                "  \"readback_failures\": %llu,\n"
                "  \"audio_rate\": %u,\n  \"audio_samples\": %llu,\n"
                "  \"audio_samples_expected\": %llu,\n  \"audio_before_start\": %llu,\n"
                "  \"audio_trimmed_at_end\": %llu,\n  \"audio_overflow\": %llu,\n"
                "  \"audio_clipped\": %llu,\n  \"audio_max_channel_lead\": %llu,\n"
                "  \"encoder_exit\": %d,\n  \"mux_exit\": %d\n}\n",
                s_failed ? "FAIL" : "PASS", s_failed ? s_fail_why : "", s_codec, s_encoder_note,
                strcmp(s_codec, "raw") == 0 ? "video.bgr0" : "video.mkv",
                mux_status == 0 ? "true" : "false", s_w, s_h, SR_VIDEOREC_FPS_NUM,
                SR_VIDEOREC_FPS_DEN, s_vcount0, (unsigned long long)s_frames,
                (unsigned long long)s_new_frames, (unsigned long long)s_repeat_frames,
                (unsigned long long)s_gap_frames, (unsigned long long)s_late_vblanks,
                (unsigned long long)s_refused_frames, (unsigned long long)s_resolve_failures,
                SR_VIDEOREC_AUDIO_RATE, (unsigned long long)s_audio_samples,
                (unsigned long long)end, (unsigned long long)s_audio_before_start,
                (unsigned long long)s_audio_trimmed, (unsigned long long)s_audio_overflow,
                (unsigned long long)s_audio_clipped, (unsigned long long)s_max_lead,
                enc_status, mux_status);
        fclose(j);
    }
    fprintf(stderr,
            "VIDEOREC: %s frames=%llu new=%llu repeat=%llu gap=%llu late_vblanks=%llu "
            "audio_samples=%llu encoder=%s%s%s\n",
            s_failed ? "FAIL" : "PASS", (unsigned long long)s_frames,
            (unsigned long long)s_new_frames, (unsigned long long)s_repeat_frames,
            (unsigned long long)s_gap_frames, (unsigned long long)s_late_vblanks,
            (unsigned long long)s_audio_samples, s_codec, s_failed ? " reason=" : "",
            s_failed ? s_fail_why : "");
    if (s_gap_frames || s_late_vblanks)
        fprintf(stderr, "VIDEOREC: the host fell behind guest time on %llu VBLANKs; those frames "
                        "repeat the previous picture (a deterministic guest clock removes this, "
                        "#2037)\n",
                (unsigned long long)(s_gap_frames + s_late_vblanks));
    fflush(stderr);
    return !s_failed;
}

#ifdef SR_VIDEOREC_SELFTEST
int sr_videorec_test_start(const char *dir, const char *codec) {
    return sr_videorec_test_start_at(dir, codec, 0u);
}

int sr_videorec_test_start_at(const char *dir, const char *codec, uint32_t start_vcount) {
    sr_videorec_test_reset();
    s_state = VR_OFF;
    if (!vr_begin(dir, codec)) return 0;
    s_start_vcount = start_vcount;
    s_state = VR_ON;
    return 1;
}

void sr_videorec_test_stats(SrVideoRecStats *out) {
    memset(out, 0, sizeof *out);
    out->frames = s_frames;
    out->new_frames = s_new_frames;
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
    if (s_raw) fclose(s_raw);
    if (s_enc.running) (void)vr_proc_finish(&s_enc);
    free(s_buf[0]);
    free(s_buf[1]);
    s_buf[0] = s_buf[1] = NULL;
    s_out = 0;
    free(s_ring);
    s_state = VR_UNREAD;
    s_dir[0] = s_codec[0] = s_encoder_note[0] = s_ffmpeg[0] = s_fail_why[0] = '\0';
    s_cur = NULL;
    s_ring = NULL;
    s_wav = s_csv = s_raw = NULL;
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
