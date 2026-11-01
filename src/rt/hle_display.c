// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The display: sceDisplay, the vblank tick and frame presentation. Extracted from hle.c on
 * 2026-10-31 (#307). */

#include "hle_internal.h"

#ifdef _WIN32
uint64_t sr_host_monotonic_ms(void) { return GetTickCount64(); }
#else
uint64_t sr_host_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
#endif

static int hle_present_trace_on(void) {
    static int enabled = -1;
    return hle_env_switch(&enabled, "SR_PRESENT_TRACE");
}
int      s_route_cols = 12, s_route_rows = 8;
int      s_route_learn;


int route_sig_bytes(void) { return s_route_cols * s_route_rows * 3; }
/* Test hook: run one observer sample exactly as the periodic sampler does and
 * report whether it produced an observation.  Exposed so the ownership rule --
 * the observer does not read the scanout until the guest owns it -- is testable
 * without a title, a route file or a GPU.  route_sample() is defined later in
 * this file, so the hook body lives beside it. */
int sr_route_test_sample(uint8_t *out);

/* sceDisplay: remember the framebuffer; vblank waits block until the next delivered vblank. */
static void dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride);

/* The PSP keeps the current scanout state separate from the next-frame
 * request.  The host presentation path is an additional, rate-limited layer;
 * it must not be confused with the guest's display latch or VBLANK state. */
DisplayFrameState s_display_active = { 0x04000000u, 512, 3 };
DisplayFrameState s_display_latched = { 0x04000000u, 512, 3 };
int s_display_latched_pending;
/* Has the guest ever established a COMPLETE scanout state -- address, stride and
 * format all supplied by sceDisplaySetFrameBuf and applied to the active state?
 *
 * Until it has, the initializers above are the only thing s_display_active holds,
 * and they are a placeholder rather than an observation of anything.  The GE can
 * (and does) register a render target at 0x04000000 before the guest's first
 * SetFrameBuf, in whatever pixel format the display list asked for -- so a reader
 * that trusts the initializer's format decodes the buffer wrongly, and a reader
 * that hands the initializer to the GPU coherence boundary trips a mismatch alarm
 * that exists to catch genuine disagreements.
 *
 * Set only where s_display_active receives a complete guest-provided state: the
 * immediate (sync=0) path, and the VBLANK that applies a latched request.  A
 * latched request alone is not enough -- it publishes stride and format at once
 * but leaves the previous scanout address in place until the latch lands. */
int s_display_configured;
uint32_t s_last_flip_vcount = 0;   /* no-frame watchdog clock (see sr_vblank_tick) */

/* sceDisplaySetFrameBuf outcome accounting.
 *
 * A stretch with no presented frame has two completely different causes, and the
 * no-frame watchdog cannot distinguish them from the flip counter alone:
 *   - the guest never asks for a flip (its render/present loop is not reaching the
 *     call), or
 *   - the guest does ask and this handler refuses the request.
 * Those point at opposite subsystems, so record which one actually happened rather
 * than leaving the diagnosis to inference. Counters only; no behavior depends on
 * them, and the cost is a few increments per call. */
static struct {
    unsigned long calls, immediate, latched, rejected;
    uint32_t last_vcount, last_addr, last_sync;
    int32_t  last_stride, last_fmt;
    uint32_t last_err, last_err_vcount, last_err_addr, last_err_sync;
    int32_t  last_err_stride, last_err_fmt;
} s_setfb;
/* Highest 600-period bucket already reported for the current no-flip stretch.
 * VCOUNT advances by whole elapsed display periods at the scheduler source
 * latch (sr_display_advance_vcount), so a single serviced tick can carry the
 * no-flip distance across a 600 boundary without ever landing on a multiple of
 * 600 -- 598 -> 602 must still report exactly once.  Comparing bucket indices
 * detects the crossing; an exact-modulus test cannot.  Reset wherever
 * s_last_flip_vcount is reset so a fresh stretch starts at bucket 0. */
static uint32_t s_watchdog_bucket = 0;

/* Dark-picture watch (fbcap.h): the VBLANK of the latest picture probe, and the open dark
 * stretch. Probes ride on presents (display_present_active); reports are made per VBLANK
 * (sr_vblank_tick), beside the no-frame watchdog. */
uint32_t s_picture_probe_vcount = 0;
SrPictureWatch s_picture_watch;

/* A resumed frame closes one observed no-frame stretch. stderr is redirected
 * to the per-run boot-event log by the native launcher, so the parent player
 * can consume this event while the game is still running. */
static void hle_watchdog_note_present(void) {
    if (s_watchdog_bucket > 0u) {
        uint32_t diff = s_vcount - s_last_flip_vcount;
        fprintf(stderr,
                "BOOT_EVENT phase=resumed observation=frame_present "
                "no_frame_vblanks=%u\n", diff);
        fflush(stderr);
    }
    s_last_flip_vcount = s_vcount;
    s_watchdog_bucket = 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* White-box view of the flip accounting for the conformance harness. The counters are
 * file-static because nothing outside this file may steer display state; the regression
 * needs to read them to prove a refused request is recorded rather than silently lost. */
void sr_display_test_flip_counts(unsigned long *calls, unsigned long *immediate,
                                 unsigned long *latched, unsigned long *rejected,
                                 uint32_t *last_err);
#endif

/* Record a refused sceDisplaySetFrameBuf and return the PSP error unchanged, so
 * every rejection path is accounted for in exactly one place. */
static uint32_t display_setframebuf_reject(uint32_t err, uint32_t addr, int32_t stride,
                                           int32_t fmt, uint32_t sync) {
    s_setfb.rejected++;
    s_setfb.last_err = err;
    s_setfb.last_err_vcount = s_vcount;
    s_setfb.last_err_addr = addr;
    s_setfb.last_err_stride = stride;
    s_setfb.last_err_fmt = fmt;
    s_setfb.last_err_sync = sync;
    return err;
}

/* A guest-VRAM dump is a publication boundary, not an ordinary present. The Vulkan
 * presenter deliberately queues its readback, so materialize only this display target
 * before exposing the legacy dump and refuse the artifact if the target cannot be made
 * current. NO_TARGET is a validated answer: it means guest memory is authoritative. */
static int snapshot_sync_ok(uint32_t fbaddr, uint32_t fmt, uint32_t stride, const char *what) {
    GeGpuFbDescriptor d = { fbaddr, fmt, stride, 480u, 272u };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc == GEGPU_SYNC_OK || rc == GEGPU_SYNC_NO_TARGET) return 1;
    fprintf(stderr,
            "%s: snapshot synchronisation FAILED (rc=%d) for addr=0x%08x fmt=%u stride=%u "
            "480x272 -- refusing to publish stale or misread guest VRAM\n",
            what, rc, fbaddr, fmt, stride);
    return 0;
}
static int display_address_valid(uint32_t addr) {
    uint32_t phys = (uint32_t)SR_PHYS(addr);
    return (phys >= 0x04000000u && phys < 0x04200000u) ||
           (phys >= 0x08000000u && phys < 0x0c000000u);
}

/* Validate the complete host-read span before the presenter dereferences it.
 * Negative PSP strides are accepted by the syscall contract but are not a
 * safe source for the top-left host read, so the host presentation is skipped
 * for those synthetic cases while the guest-visible state remains intact. */
static int display_host_span_valid(const DisplayFrameState *fb) {
    if (!fb->addr) return 1;
    if (fb->stride <= 0 || fb->fmt < 0 || fb->fmt > 3) return 0;
    uint32_t bpp = fb->fmt == 3 ? 4u : 2u;
    uint32_t row_bytes, bytes, last;
    if (!sr_size_mul_ok((uint32_t)fb->stride, bpp, &row_bytes) ||
        !sr_size_mul_ok(row_bytes, 272u, &bytes) || bytes == 0u)
        return 0;
    uint32_t phys = (uint32_t)SR_PHYS(fb->addr);
    if (!sr_size_add_ok(phys, bytes - 1u, &last)) return 0;
    if (phys >= 0x04000000u && phys < 0x04200000u)
        if (last >= 0x04200000u) return 0;
    if (phys >= 0x08000000u && phys < 0x0c000000u)
        if (last >= 0x0c000000u) return 0;
    return sr_guest_span_readable(fb->addr, bytes);
}

static void display_present_frame(void) {
    if (!gui_on() || !s_display_active.addr) return;
    if (!display_host_span_valid(&s_display_active)) {
        fprintf(stderr, "DISPLAY_PRESENT: refusing invalid span addr=0x%08x stride=%d fmt=%d\n",
                s_display_active.addr, s_display_active.stride, s_display_active.fmt);
        return;
    }
    int presented = gui_present(s_display_active.addr, s_display_active.fmt,
                                (uint32_t)s_display_active.stride);
    if (presented) {
        if (sr_perf_enabled) sr_perf_present_frame(s_display_active.addr, s_vcount);
        SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_PRESENT, SR_FLIGHT_KIND_PRESENT_FRAME,
                               s_display_active.addr, (uint32_t)s_display_active.fmt,
                               (uint32_t)s_display_active.stride, s_vcount);
        if (hle_present_trace_on())
            fprintf(stderr, "HOST_PRESENT_SUBMITTED f=%u buf=0x%08x fmt=%d stride=%d\n",
                    s_vcount, s_display_active.addr, s_display_active.fmt,
                    s_display_active.stride);
        ge_transition_trace_check("present", s_vcount);
    }
}

/* A probe verdict that ends a reported dark stretch is a boot event for the player. */
static void picture_probe_resumed(int verdict, uint32_t brightest) {
    if (verdict != SR_PICTURE_WATCH_RESUMED) return;
    fprintf(stderr, "BOOT_EVENT phase=resumed observation=picture brightest=%u\n", brightest);
    fflush(stderr);
}

/* Feed the result of a probe whose readback was still on the GPU when its present returned
 * (fbcap.h, deferred probe results), under the VBLANK the probe was taken. With block, wait
 * for the GPU: only when the result could change what the watch does next. */
static void picture_probe_collect(int block) {
    uint32_t brightest = 0u;
    int verdict = sr_picture_watch_collect(&s_picture_watch, block, &brightest);
    picture_probe_resumed(verdict, brightest);
}

/* Present the active framebuffer, then close this frame's capture (fbcap.h): the presenter
 * that showed the frame has already serviced an armed capture, and an arm nobody serviced
 * (a skipped output slot, a presenter that declined the frame) resolves here as "nothing
 * attempted", so a later present can never publish this frame's path with newer pixels. */
static void display_present_active(void) {
    /* Dark-picture watch (fbcap.h): every SR_PICTURE_PROBE_EVERY VBLANKs, a present no file
     * capture wanted carries a picture probe. A presenter that serves it from host memory
     * does so inside this present, so its result is taken right after; one that reads it
     * back from the GPU leaves it in flight, and its result is collected without waiting by
     * a later present or VBLANK. A probe still in flight when the next one is due (a GPU
     * more than SR_PICTURE_PROBE_EVERY VBLANKs behind) is waited for, so the cadence holds. */
    picture_probe_collect(0);
    if (s_picture_watch.pending && s_vcount - s_picture_probe_vcount >= SR_PICTURE_PROBE_EVERY)
        picture_probe_collect(1);
    int probe = 0;
    if (!s_picture_watch.pending && !sr_capture_is_armed() && gui_on() &&
        s_display_active.addr &&
        s_vcount - s_picture_probe_vcount >= SR_PICTURE_PROBE_EVERY &&
        display_host_span_valid(&s_display_active))
        probe = sr_capture_arm_probe();
    /* Ace Mode's 30 Hz detector (ace.h) sees every flip here, before the presenter's own slot
     * rule can drop a second flip in one VBLANK. Off, nothing is read. */
    if (g_ace_on && s_display_active.addr) {
        uint32_t bytes = (uint32_t)(s_display_active.stride > 0 ? s_display_active.stride : 512) *
                         272u * (s_display_active.fmt == 3 ? 4u : 2u);
        (void)ace_flip(s_display_active.addr & 0x1FFFFFu, bytes, s_vcount,
                       sr_host_monotonic_ms() * 1000000ull);
    }
    display_present_frame();
    sr_capture_cancel();
    uint32_t brightest;
    if (probe && sr_capture_probe_take(&brightest)) {
        s_picture_probe_vcount = s_vcount;
        picture_probe_resumed(sr_picture_watch_probe(&s_picture_watch, s_vcount, brightest),
                              brightest);
    } else if (probe && sr_capture_probe_in_flight()) {
        s_picture_probe_vcount = s_vcount;
        sr_picture_watch_probe_sent(&s_picture_watch, s_vcount);
    }
}

/* ---- route observation (issue #64) ------------------------------------------------
 *
 * Decode one presented pixel. fmt: 0=5650, 1=5551, 2=4444, 3=8888 -- the same table
 * dump_fb_fmt writes its PPMs from, factored out so the route signature and the capture
 * files can never disagree about what a frame looked like. */
static void fb_decode_px(uint32_t fbaddr, int fmt, uint32_t stride, int x, int y,
                         unsigned char rgb[3]) {
    if (fmt == 3) {
        uint32_t p = MEM_R32(fbaddr + (uint32_t)(y * (int)stride + x) * 4);
        rgb[0] = p & 0xFF; rgb[1] = (p >> 8) & 0xFF; rgb[2] = (p >> 16) & 0xFF;
        return;
    }
    uint16_t p = MEM_R16(fbaddr + (uint32_t)(y * (int)stride + x) * 2);
    if (fmt == 1) {
        rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((p >> 5) & 0x1F) * 255 / 31);
        rgb[2] = (unsigned char)(((p >> 10) & 0x1F) * 255 / 31);
    } else if (fmt == 2) {
        rgb[0] = (unsigned char)(((p) & 0xF) * 17);
        rgb[1] = (unsigned char)(((p >> 4) & 0xF) * 17);
        rgb[2] = (unsigned char)(((p >> 8) & 0xF) * 17);
    } else {
        rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
    }
}

/* The presenter queues its readback, so guest VRAM has to be made current before it can
 * be believed -- the same boundary snapshot_sync_ok() enforces for published captures.
 * Reporting is bounded because a route samples repeatedly and a systematic sync failure
 * would otherwise bury the ROUTE_FAIL that follows it. */
static int route_sync_fb(void) {
    GeGpuFbDescriptor d = { s_display_active.addr, (uint32_t)s_display_active.fmt,
                            (uint32_t)s_display_active.stride, 480u, 272u };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc == GEGPU_SYNC_OK || rc == GEGPU_SYNC_NO_TARGET) return 1;
    static int warned = 0;
    if (warned < 3) {
        warned++;
        fprintf(stderr, "ROUTE: framebuffer synchronisation failed (rc=%d); no observation "
                        "this sample\n", rc);
    }
    return 0;
}

/* Mean R/G/B of a fixed 4x4 subgrid of each cell. Subsampling rather than averaging every
 * pixel keeps the observation cheap enough that it cannot pace the run it is measuring:
 * at the default 12x8 grid this reads 1536 pixels, once every SAMPLE_EVERY vblanks, and
 * only while a WAIT or EXPECT is pending. */
int route_sample(uint8_t *out) {
    /* Nothing to observe before the guest owns the scanout state: see
     * s_display_configured.  This is deliberately checked ahead of
     * route_sync_fb() so the observer never hands the GPU coherence boundary a
     * placeholder descriptor -- doing so raised a `snapshot sync refused'
     * mismatch during ordinary boot, which reads exactly like the genuine
     * disagreement that check exists to report. */
    if (!s_display_configured) return 0;
    if (!s_display_active.addr || !display_host_span_valid(&s_display_active)) return 0;
    if (!route_sync_fb()) return 0;
    uint32_t stride = (uint32_t)s_display_active.stride;
    if (!stride) stride = 512;
    int fmt = s_display_active.fmt;
    int k = 0;
    for (int cy = 0; cy < s_route_rows; cy++)
        for (int cx = 0; cx < s_route_cols; cx++) {
            unsigned sum[3] = { 0, 0, 0 };
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int x = (cx * 480) / s_route_cols + (sx * (480 / s_route_cols)) / 4;
                    int y = (cy * 272) / s_route_rows + (sy * (272 / s_route_rows)) / 4;
                    if (x > 479) x = 479;
                    if (y > 271) y = 271;
                    unsigned char rgb[3];
                    fb_decode_px(s_display_active.addr, fmt, stride, x, y, rgb);
                    sum[0] += rgb[0]; sum[1] += rgb[1]; sum[2] += rgb[2];
                }
            out[k++] = (uint8_t)(sum[0] / 16);
            out[k++] = (uint8_t)(sum[1] / 16);
            out[k++] = (uint8_t)(sum[2] / 16);
        }
    return 1;
}

int sr_route_test_sample(uint8_t *out) { return route_sample(out); }

/* SR_ROUTE_LEARN: print the signature of one frame. Authoring a checkpoint means looking
 * at a captured frame and deciding what screen it is, so the signature has to be emitted
 * for exactly the vblanks that are captured -- otherwise the author is transcribing a
 * signature from a frame they never saw. Called from the periodic sampler and again from
 * the capture path, deduplicated by vblank. */
void route_learn_emit(uint32_t v, const uint8_t *sig) {
    static uint32_t last = UINT32_MAX;
    uint8_t local[ROUTE_SIG_MAX];
    char hex[ROUTE_SIG_MAX * 2 + 1];
    int n = route_sig_bytes();
    if (v == last) return;
    if (!sig) {
        if (!route_sample(local)) return;
        sig = local;
    }
    last = v;
    for (int i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", sig[i]);
    fprintf(stderr, "ROUTE_SIG v=%u %s\n", v, hex);
}

/* ---- present-truthful frame capture (issue #57) -----------------------------------
 *
 * The capture-slot policy (fbcap_policy.h) decides who owns the next present:
 *   - SR_FBDUMP owns the first present whose vblank reaches the SR_FBDUMP threshold and
 *     publishes "present_source.ppm"; the FBDUMP block below then exits with a policy
 *     verdict (success only if the capture was actually published).
 *   - SR_FBSNAP owns every present selected by the <N>/<AFTER>/<WINDOWS> gates and
 *     publishes build/snapshots/frame_%04u.ppm (rotating) or frame_v<vcount>.ppm
 *     (windows). The legacy VRAM-side snap_*.ppm oracle file is kept beside it with its
 *     exact historical naming, so existing routes and tooling are unaffected.
 *
 * The arm MUST run before the present call so the recorded frame is exactly the one
 * being presented. Whichever presenter shows the frame (Vulkan window, GDI window or the
 * headless offscreen sink) publishes it through the presenter-neutral capture service
 * (fbcap.h), and display_present_active() resolves an arm no presenter serviced, so a
 * published capture always corresponds to a presented frame. */
#define SR_FBSNAP_MAX_WINDOWS 8
static uint32_t s_fbsnap_win_lo[SR_FBSNAP_MAX_WINDOWS], s_fbsnap_win_hi[SR_FBSNAP_MAX_WINDOWS];
static int s_fbsnap_win_n = 0;
static int s_fbsnap_win_parsed;  /* SR_FBSNAP_WINDOWS is read once per process */
static int s_fbsnap_every = -1;  /* cadence latched by the first FBSNAP-owned present */
static uint32_t s_fbsnap_after;  /* SR_FBSNAP_AFTER, latched with the cadence */
static uint32_t s_fbsnap_last;   /* vcount of the last FBSNAP arm */
static char s_fbcap_armed[128];  /* path armed for the CURRENT frame's present ("" = none) */
static char s_fbcap_legacy[64];  /* legacy snap_*.ppm path for the same frame ("" = none) */
uint32_t s_vramdump_vblanks[SR_VRAMDUMP_MAX_VBLANKS];
unsigned s_vramdump_count;
unsigned s_vramdump_done;
static char s_vramdump_dir[768];

void vramdump_init_once(void) {
    static int initialized;
    const char *selection, *directory;
    const char *cursor;
    if (initialized) return;
    initialized = 1;
    selection = getenv("SR_VRAMDUMP");
    if (!selection || !selection[0]) return;
    directory = getenv("SR_VRAMDUMP_DIR");
    if (!directory || !directory[0] || strlen(directory) >= sizeof s_vramdump_dir) {
        fprintf(stderr, "VRAMDUMP CONFIG_FAIL (SR_VRAMDUMP_DIR must name an existing output directory)\n");
        return;
    }
    memcpy(s_vramdump_dir, directory, strlen(directory) + 1u);
    cursor = selection;
    while (*cursor) {
        char *end = NULL;
        unsigned long value;
        if (s_vramdump_count >= SR_VRAMDUMP_MAX_VBLANKS || *cursor < '0' || *cursor > '9')
            goto invalid;
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno == ERANGE || end == cursor || value > UINT32_MAX || (*end && *end != ','))
            goto invalid;
        for (unsigned i = 0; i < s_vramdump_count; i++)
            if (s_vramdump_vblanks[i] == (uint32_t)value) goto invalid;
        s_vramdump_vblanks[s_vramdump_count++] = (uint32_t)value;
        if (!*end) break;
        cursor = end + 1;
        if (!*cursor) goto invalid;
    }
    if (!s_vramdump_count) goto invalid;
    fprintf(stderr, "VRAMDUMP enabled selections=%u\n", s_vramdump_count);
    return;

invalid:
    s_vramdump_count = 0;
    fprintf(stderr, "VRAMDUMP CONFIG_FAIL (expected up to %u unique decimal vblank values)\n",
            SR_VRAMDUMP_MAX_VBLANKS);
}

static void vramdump_try_present(uint32_t vcount, const DisplayFrameState *fb) {
    vramdump_init_once();
    if (!s_vramdump_count || !fb) return;
    /* PASS must only ever mean a presented frame: apply the conditions
     * display_present_active() applies before it hands a frame to the presenter. A
     * vblank where it would decline stays selected and is reported NOT_CAPTURED. */
    if (!gui_on() || !fb->addr || !display_host_span_valid(fb)) return;
    for (unsigned i = 0; i < s_vramdump_count; i++) {
        unsigned bit = 1u << i;
        if (s_vramdump_vblanks[i] != vcount || (s_vramdump_done & bit)) continue;
        s_vramdump_done |= bit;
        if (ge_vramdump_write(s_vramdump_dir, vcount, fb->addr,
                              (uint32_t)fb->stride, (uint32_t)fb->fmt))
            fprintf(stderr, "VRAMDUMP vblank=%u PASS\n", vcount);
        else
            fprintf(stderr, "VRAMDUMP vblank=%u FAIL (see diagnostic above)\n", vcount);
        break;
    }
}

static void fbcap_parse_windows_once(void) {
    if (s_fbsnap_win_parsed) return;
    s_fbsnap_win_parsed = 1;
    const char *w = getenv("SR_FBSNAP_WINDOWS");
    if (!w || !w[0]) return;
    const char *p = w;
    while (*p && s_fbsnap_win_n < SR_FBSNAP_MAX_WINDOWS) {
        char *end = NULL;
        unsigned long lo = strtoul(p, &end, 10);
        if (end == p) break;
        if (*end != '-') { p = end; while (*p == ',') p++; continue; }
        p = end + 1;
        unsigned long hi = strtoul(p, &end, 10);
        if (end == p) break;
        p = end;
        s_fbsnap_win_lo[s_fbsnap_win_n] = (uint32_t)lo;
        s_fbsnap_win_hi[s_fbsnap_win_n] = (uint32_t)hi;
        s_fbsnap_win_n++;
        while (*p == ',') p++;
    }
    for (int i = 0; i < s_fbsnap_win_n; i++)
        fprintf(stderr, "FBSNAP_WINDOW[%d] = %u..%u\n", i, s_fbsnap_win_lo[i], s_fbsnap_win_hi[i]);
    if (s_fbsnap_win_n == 0)
        fprintf(stderr, "FBSNAP_WINDOWS: could not parse '%s' -- no windows active\n", w);
}

/* Decide per present whether the NEXT present must be recorded, and arm it before the
 * present call. Host-side gate only: no guest work is skipped, no timing is changed,
 * and the frames that do get captured are byte-identical to an ungated run. */
static const char *fbcap_arm_for_present(uint32_t vcount, const DisplayFrameState *fb,
                                         uint32_t sync, int framebuf_set) {
    s_fbcap_armed[0] = '\0';
    s_fbcap_legacy[0] = '\0';
    if (sync != 0u) return NULL;
    /* SR_FBSNAP_WINDOWS alone selects FBSNAP (every present inside the windows), so the
     * windows are parsed before the owner decision, which uses the same effective cadence
     * as the gate below (sr_fbcap_snap_every). */
    fbcap_parse_windows_once();
    int fbsnap_every = sr_fbcap_snap_every(getenv("SR_FBSNAP"), s_fbsnap_win_n > 0);
    int owner = sr_fbcap_owner(sr_fbcap_env_on("SR_FBDUMP"), fbsnap_every > 0);
    if (owner == SR_FBCAP_NONE) return NULL;
    /* Arm only a frame display_present_active() will hand to a presenter -- the same gate
     * vramdump_try_present() applies. With no presenter (no --gui, or none initialised)
     * there is nothing to capture, so neither the capture nor its legacy VRAM-side
     * snapshot is taken. */
    if (!gui_on() || !framebuf_set || !display_host_span_valid(fb)) return NULL;
    if (owner == SR_FBCAP_FBDUMP) {
        const char *fd = getenv("SR_FBDUMP");
        if (!fd || vcount < (uint32_t)atoi(fd)) return NULL;
        if (!sr_fbcap_path(SR_FBCAP_FBDUMP, 0, s_fbcap_armed, sizeof s_fbcap_armed))
            return NULL;
        if (!sr_capture_arm(s_fbcap_armed)) {
            /* A refused arm must not leave a path behind: the report below would
             * otherwise print a stale result from an earlier capture. */
            s_fbcap_armed[0] = '\0';
            s_fbcap_legacy[0] = '\0';
            return NULL;
        }
        return s_fbcap_armed;
    }
    /* SR_FBSNAP: <N> every / AFTER / WINDOWS gates. */
    {
        if (s_fbsnap_every < 0) {    /* only an FBSNAP owner gets here: fbsnap_every >= 1 */
            s_fbsnap_every = fbsnap_every;
            const char *a = getenv("SR_FBSNAP_AFTER");
            unsigned long av = a && a[0] ? strtoul(a, NULL, 10) : 0ul;
            s_fbsnap_after = av > UINT32_MAX ? UINT32_MAX : (uint32_t)av;
        }
        const int fs = s_fbsnap_every;
        int in_window = 1;
        if (s_fbsnap_win_n > 0) {
            in_window = 0;
            for (int i = 0; i < s_fbsnap_win_n; i++)
                if (vcount >= s_fbsnap_win_lo[i] && vcount <= s_fbsnap_win_hi[i]) {
                    in_window = 1; break;
                }
        }
        if (!in_window || vcount < s_fbsnap_after || vcount - s_fbsnap_last < (uint32_t)fs)
            return NULL;
        s_fbsnap_last = vcount;
        if (s_fbsnap_win_n > 0)
            snprintf(s_fbcap_armed, sizeof s_fbcap_armed, "frame_v%u.ppm", vcount);
        else if (!sr_fbcap_path(SR_FBCAP_FBSNAP, vcount, s_fbcap_armed, sizeof s_fbcap_armed))
            return NULL;
        if (s_fbsnap_win_n > 0)
            snprintf(s_fbcap_legacy, sizeof s_fbcap_legacy, "snap_v%u.ppm", vcount);
        else
            snprintf(s_fbcap_legacy, sizeof s_fbcap_legacy, "snap_%u.ppm",
                     (vcount / (uint32_t)fs) % 8u);
        if (!sr_capture_arm(s_fbcap_armed)) {
            s_fbcap_armed[0] = '\0';
            s_fbcap_legacy[0] = '\0';
            return NULL;
        }
        return s_fbcap_armed;
    }
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Selftest-only: forget the latched SR_FBSNAP configuration (windows, cadence, AFTER and
 * the last armed vcount) so the executable regression can drive a fresh configuration
 * through the production arm path. Production latches it once per process. */
void sr_fbcap_test_reset_config(void) {
    s_fbsnap_win_parsed = 0;
    s_fbsnap_win_n = 0;
    s_fbsnap_every = -1;
    s_fbsnap_after = 0;
    s_fbsnap_last = 0;
}
#endif

static uint32_t h_DisplaySetFrameBuf(CpuState *s) {
    static int first_present = 1;
    uint32_t addr = A0;
    int32_t stride = (int32_t)A1;
    int32_t fmt = (int32_t)A2;
    uint32_t sync = A3;
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_PRESENT, SR_FLIGHT_KIND_PRESENT_SET_FRAMEBUF,
                           addr, (uint32_t)fmt, (uint32_t)stride, s_vcount);
    if (ge_log_on())
        fprintf(stderr, "DISPLAY_SET_FB: buf=0x%08x stride=%d fmt=%d sync=%u vcount=%u\n",
                addr, stride, fmt, sync, s_vcount);

    s_setfb.calls++;
    s_setfb.last_vcount = s_vcount;
    s_setfb.last_addr = addr;
    s_setfb.last_stride = stride;
    s_setfb.last_fmt = fmt;
    s_setfb.last_sync = sync;

    if (sync > 1u)                           /* SCE_KERNEL_ERROR_INVALID_MODE */
        return display_setframebuf_reject(0x80000107u, addr, stride, fmt, sync);
    if (addr && !display_address_valid(addr)) /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
        return display_setframebuf_reject(0x80000103u, addr, stride, fmt, sync);
    if ((addr & 0x0fu) != 0u)                /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
        return display_setframebuf_reject(0x80000103u, addr, stride, fmt, sync);
    if (((uint32_t)stride & 0x3fu) != 0u || (stride == 0 && addr != 0u))
        return display_setframebuf_reject(0x80000104u, addr, stride, fmt, sync);
    if (fmt < 0 || fmt > 3)                  /* SCE_KERNEL_ERROR_INVALID_FORMAT */
        return display_setframebuf_reject(0x80000108u, addr, stride, fmt, sync);
    if (sync == 0u &&
        (stride != s_display_latched.stride || fmt != s_display_latched.fmt))
        return display_setframebuf_reject(0x80000107u, addr, stride, fmt, sync);

    DisplayFrameState requested = { addr, stride, fmt };
    if (sync == 0u) {
        s_setfb.immediate++;
        /* PSP names sync=0 IMMEDIATE.  The host has no scanline clock, so this
         * updates the guest-visible/current state immediately; sync=1 remains
         * the modeled next-frame latch applied at VBLANK. */
        s_display_active = requested;
        s_display_latched = requested;
        s_display_latched_pending = 0;
        s_display_configured = 1;
        s_framebuf = addr;
        hle_watchdog_note_present();
        /* Issue #57: arm any due present capture BEFORE the present so the recorded
         * frame is exactly the one being presented. */
        fbcap_arm_for_present(s_vcount, &s_display_active, 0u, s_framebuf != 0);
        vramdump_try_present(s_vcount, &s_display_active);
        psmf_av_video_presented(s_display_active.addr, s_display_active.stride, s_display_active.fmt);
        display_present_active();
    } else {
        s_setfb.latched++;
        s_display_latched = requested;
        s_display_latched_pending = 1;
        /* PSP autotests observe format/stride immediately, but the address
         * remains the active scanout address until VBLANK starts. */
        s_display_active.stride = stride;
        s_display_active.fmt = fmt;
    }
    if (first_present) {
        first_present = 0;
        fprintf(stderr, "BOOT_EVENT phase=display_flip vcount=%u buffer=0x%08x stride=%u format=%u\n",
                s_vcount, A0, A1, A2);
    }
    /* Frame-delivery diagnostic (improvement #1): the buffer the game presents should be
     * the one the GE just drew into. A mismatch means the presented framebuffer holds
     * stale/empty VRAM (black screen) while the real scene sits in another buffer. */
    {
        static int fbd = -1;
        if (fbd < 0) { const char *e = getenv("SR_FBDIAG"); fbd = e ? 1 : 0; }
        if (fbd) {
            uint32_t ge_fb = ge_framebuffer();
            if (ge_fb && ge_fb != s_framebuf)
                fprintf(stderr, "FRAMEBUF MISMATCH: presented=0x%08x ge_draw_target=0x%08x\n", s_framebuf, ge_fb);
        }
    }
    /* SR_GEWATCH: interleave presents with GELIST lines to expose draw-vs-present ordering. */
    {
        static int gw = -1, gwa = 0;
        if (gw < 0) { gw = getenv("SR_GEWATCH") ? 1 : 0;
                      const char *p = getenv("SR_GEWATCH_AFTER"); gwa = p ? atoi(p) : 0; }
        if (gw && sync == 0u && s_vcount >= (uint32_t)gwa)
            fprintf(stderr, "PRESENT f=%u buf=0x%08x fmt=%d stride=%d\n",
                    s_vcount, s_display_active.addr, s_display_active.fmt,
                    s_display_active.stride);
    }
    /* The active state was presented above. A sync=1 request is intentionally
     * deferred until sr_vblank_tick applies the pending scanout state. */
    /* SR_FBSNAP report (issue #57): fbcap_arm_for_present() above already armed the
     * present-truthful capture for this frame (path in s_fbcap_armed); the presenter
     * resolved it before display_present_active() returned. The
     * legacy VRAM-side oracle keeps its exact historical naming: rotating snap_%u.ppm
     * (8 most recent kept) without windows, snap_v<vcount>.ppm when windows are set, so
     * existing routes and tooling are unaffected. Host-side gate only: no guest work is
     * skipped, no timing changes, and the frames that are captured are byte-identical
     * to an ungated run. */
    if (s_fbcap_armed[0]) {
        if (s_fbcap_legacy[0]) {
            if (snapshot_sync_ok(s_display_active.addr, (uint32_t)s_display_active.fmt,
                                  (uint32_t)s_display_active.stride, "FBSNAP")) {
                dump_fb_fmt(s_fbcap_legacy, s_display_active.addr, s_display_active.fmt,
                            (uint32_t)s_display_active.stride);
                fprintf(stderr, "FBSNAP f=%u -> %s\n", s_vcount, s_fbcap_legacy);
                /* Issue #64: pair the captured frame with its route signature so a new
                 * checkpoint is transcribed from the frame the author actually looked at. */
                if (s_route_learn) route_learn_emit(s_vcount, NULL);
            } else {
                fprintf(stderr, "FBSNAP f=%u -> SKIPPED (synchronisation failed)\n", s_vcount);
            }
        }
        int cres = sr_capture_result();
        if (cres != 0) {
            fprintf(stderr, "FBSNAP f=%u swapchain capture -> %s (result=%d)\n",
                    s_vcount, s_fbcap_armed, cres);
        } else {
            /* No presenter serviced this frame's arm (the output cap dropped the present):
             * display_present_active() cancelled it, so it can neither be serviced by a
             * later frame nor reported with a stale result. */
            fprintf(stderr, "FBSNAP f=%u swapchain capture -> SKIPPED (no present serviced this frame)\n",
                    s_vcount);
        }
        s_fbcap_armed[0] = '\0';
        s_fbcap_legacy[0] = '\0';
    }
    /* The buffer handed to SetFrameBuf is a freshly-completed frame. With SR_FBDUMP=<N>, once N
     * frames have elapsed, the pre-present arm above recorded exactly this presented buffer in
     * present_source.ppm and the process exits with a policy verdict (issue #57): success(0)
     * only if the present-truthful capture really was published, failure(1) otherwise. */
    {
        static int fbdu = -1; static int fbdu_n = 0;
        if (fbdu < 0) {
            const char *fd = getenv("SR_FBDUMP");
            fbdu = sr_fbcap_env_on("SR_FBDUMP");
            fbdu_n = fd ? atoi(fd) : 0;
        }
        if (sync == 0u && fbdu && s_fbcap_armed[0] && s_framebuf &&
            s_vcount >= (uint32_t)fbdu_n &&
            display_host_span_valid(&s_display_active)) {
            int snap_ok = snapshot_sync_ok(s_display_active.addr,
                                            (uint32_t)s_display_active.fmt,
                                            (uint32_t)s_display_active.stride, "SR_FBDUMP");
            if (snap_ok)
                dump_fb_fmt("fb_present.ppm", s_display_active.addr, s_display_active.fmt,
                            (uint32_t)s_display_active.stride);
            /* Also snapshot the whole 2MB eDRAM so any rendered region can be found regardless of which
             * buffer/stride/format the game settled on. */
            FILE *raw = fopen("edram.bin", "wb");
            if (raw) { for (uint32_t a = 0x04000000; a < 0x04200000; a += 4) { uint32_t w = MEM_R32(a); fwrite(&w, 4, 1, raw); } fclose(raw); }
            sr_trace_close();
            fprintf(stderr, "presented frame %u: buf=0x%08x fmt=%u stride=%u ge_pixels=%lu tex_samples=%lu tex_nonzero=%lu\n",
                    s_vcount, s_display_active.addr, s_display_active.fmt,
                    (uint32_t)s_display_active.stride, g_ge_pixels, g_tex_samples, g_tex_nonzero);
            { extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
              fprintf(stderr, "mpeg: ringPut=%lu getAvcAu=%lu avcDecode=%lu noData=%lu\n",
                      g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata); }
            sr_dump_calls();
            sched_dump_threads();
            /* Only a capture serviced by this frame's present may affect the exit
             * verdict. An unserviced arm is a failed attempt, never a stale success. */
            int cres = sr_capture_result();
            fprintf(stderr, "present capture result=%d (source=%s)%s\n", cres,
                    sr_capture_source_label(),
                    cres == 0 ? " (not serviced: no present this frame)" : "");
            if (!snap_ok) {
                fprintf(stderr, "SR_FBDUMP: no trustworthy framebuffer snapshot was written\n");
                sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, 0u, 1u);
                sr_compat_report();
                sr_perf_shutdown();
                _Exit(1);
            }
            {
                int capture_exit_status = sr_fbcap_exit_status(SR_FBCAP_FBDUMP, cres);
                sr_flight_exit((uint32_t)capture_exit_status);
                psmf_av_final("capture_exit");
                sr_compat_report();
                sr_perf_shutdown();
                _Exit(capture_exit_status);
            }
        }
    }
    return 0;
}
static uint32_t h_DisplayGetFrameBuf(CpuState *s) {
    if (A3 > 1u) return 0x80000107u;
    const DisplayFrameState *fb = A3 == 1u ? &s_display_latched : &s_display_active;
    if (A0 && !sr_guest_span_writable(A0, 4u)) return 0x80000103u;
    if (A1 && !sr_guest_span_writable(A1, 4u)) return 0x80000103u;
    if (A2 && !sr_guest_span_writable(A2, 4u)) return 0x80000103u;
    if (A0) MEM_W32(A0, fb->addr);
    if (A1) MEM_W32(A1, (uint32_t)fb->stride);
    if (A2) MEM_W32(A2, (uint32_t)fb->fmt);
    return 0;
}

/* Dump a PSP framebuffer to a binary PPM. fmt: 0=5650, 1=5551, 2=4444, 3=8888. */
static void dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (!stride) stride = 512;
    /* One buffered write instead of 480*272 three-byte fwrite calls (130,560 stdio
     * round-trips per capture). Byte-for-byte the same file: same header, same pixel
     * order, same three bytes per pixel -- fb_decode_px writes all three on every
     * branch, so no byte of the buffer is left stale from a previous capture.
     * The extents are named once so the buffer and the loops cannot drift apart. */
    enum { FB_CAP_W = 480, FB_CAP_H = 272 };
    static unsigned char ppm_buf[FB_CAP_W * FB_CAP_H * 3];
    _Static_assert(sizeof(ppm_buf) == (size_t)FB_CAP_W * FB_CAP_H * 3,
                   "PPM buffer must hold exactly one RGB frame");
    unsigned char *dst = ppm_buf;
    for (int y = 0; y < FB_CAP_H; y++) {
        for (int x = 0; x < FB_CAP_W; x++) {
            fb_decode_px(fbaddr, fmt, stride, x, y, dst);
            dst += 3;
        }
    }
    /* The loops filled exactly the buffer; nothing short-writes a partial frame. */
    if (dst != ppm_buf + sizeof(ppm_buf)) { fclose(f); return; }
    fprintf(f, "P6\n%d %d\n255\n", FB_CAP_W, FB_CAP_H);
    fwrite(ppm_buf, 1, sizeof(ppm_buf), f);
    fclose(f);
    fprintf(stderr, "dumped framebuffer 0x%08x fmt=%d stride=%u -> %s\n", fbaddr, fmt, stride, path);
}
/* (dump_fb wrapper dropped -- dump_fb_fmt handles all paths.) */

/* The two display waits, split according to what hardware actually does.
 *
 * Measured on PSP-3001/6.61-ARK (probe case `display-wait-late`, record
 * PSP-DISPLAY-002). At every phase OUTSIDE the vblank interval -- including
 * 1.25, 1.75 and 2.5 periods late, so with whole edges already missed -- the two
 * calls are indistinguishable: 240/240 trials each blocked to the next start
 * edge, advanced VCOUNT by exactly 1, and returned 0. They differ in exactly one
 * cell: called from INSIDE the vblank interval, sceDisplayWaitVblank returns in
 * 3..5 us (syscall overhead, no block) with the value 1, while
 * sceDisplayWaitVblankStart waits a full 16.669..16.693 ms period and returns 0.
 *
 * The interrupt/dispatch-disabled rejection is unchanged and still precedes
 * everything: both NIDs return CAN_NOT_WAIT there (matrix L26/L27 and L34/L35,
 * both CONFORMS). Nothing downstream runs on that path -- no thread blocks on
 * VBLANK_WAIT_OBJ and scheduler virtual time is untouched.
 *
 * Still NOT modelled, and deliberately so: the interrupt-context split, where
 * sceDisplayWaitVblank succeeds with 1 (L323) while sceDisplayWaitVblankStart
 * returns ILLEGAL_CONTEXT (L325). Those cells are `NOT RUN` against this runtime
 * in docs/PSP_INTR_WAITS_MATRIX.md and belong to the interrupt-context work.
 * The callback variants below use the same measured wait split, while the
 * scheduler keeps servicing callbacks until a delivered VBLANK satisfies it. */
static uint32_t h_DisplayWaitVblankStart(CpuState *s) {
    (void)s;
    if (ge_log_on()) fprintf(stderr, "HLE: WaitVblankStart (vcount=%u)\n", s_vcount);
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_wait_vblank_start();
    return 0;
}

/* The public multi-wait contract blocks through the requested number of
 * vertical periods, then returns at the following VBLANK start. */
static uint32_t h_DisplayWaitVblankStartMulti(CpuState *s) {
    (void)s;
    uint32_t periods = A0;
    if (periods == 0u) {
        /* The waits-matrix transcription records INVALID_VALUE (0x800001fe)
         * for an invalid count before the blocking-context result. */
        return 0x800001feu;
    }
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    if (sched_current_uid() == 0u) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    for (uint32_t period = 0; period < periods; period++)
        sched_wait_vblank_start();
    return 0;
}

static uint32_t h_DisplayWaitVblank(CpuState *s) {
    (void)s;
    if (ge_log_on()) fprintf(stderr, "HLE: WaitVblank (vcount=%u)\n", s_vcount);
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    /* 1 when the caller was already inside the interval and did not block. */
    return (uint32_t)sched_wait_vblank();
}

static uint32_t h_DisplayWaitVblankStartCB(CpuState *s) {
    (void)s;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    int result = sched_wait_vblank_cb(1);
    return result < 0 ? SCE_KERNEL_ERROR_CAN_NOT_WAIT : 0u;
}

static uint32_t h_DisplayWaitVblankCB(CpuState *s) {
    (void)s;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    int result = sched_wait_vblank_cb(0);
    return result < 0 ? SCE_KERNEL_ERROR_CAN_NOT_WAIT : (uint32_t)result;
}
static uint32_t h_DisplayGetMode(CpuState *s) {
    if (A0) MEM_W32(A0, 0);  /* mode 0 */
    if (A1) MEM_W32(A1, 480); /* width */
    if (A2) MEM_W32(A2, 272); /* height */
    return 0;
}
uint32_t sr_get_ge_status(void) {
    /* Real PSP GE_STATUS layout (PPSSPP GPU.h GE_STATUS_*):
     *   bit 0x01  BUSY        -- 0 = GE is idle, ready to accept a new list
     *   bit 0x02  DOFLUSH     -- last DRAW op had a flush flag
     *   bit 0x04  DRAWEND     -- a DRAW semantics reached the GE end-of-list
     *   bit 0x08  FINISHEND   -- last FINISH/END command reached by the streamer
     *   bit 0x10  BP2 / intr  -- interrupt pending
     *   bit 0x20  VBLANK      -- currently inside the vsync interval
     *
     * The PSP command-streamer thread (uid 0x115 in our boot) polls this register and gates
     * workload submission on bits 0..4, NOT just on VBLANK. Returning 0x00 outside vblank
     * told the streamer "GE is busy / nothing finished", so it spun forever waiting for a
     * BUSY-clear handshake that only a real HW-cleared FINISH would issue.
     *
     * We bake on the "ready/IDLE" mask (bits 0x08 FINISHEND + 0x10 BP2           = 0x18)
     * and OR in VBLANK on the 1.5 ms cycle. bit 0x01 BUSY stays cleared, so any future
     * "is the GE idle?" check passes. If the guest ever reads this register mid-frame
     * the FINISHEND flag stays set; the previous-frame-finished-so-flip semantics that the
     * driver relies on keep working. */
    uint32_t vblank   = sched_display_is_vblank() ? 0x20u : 0x0u;
    return 0x18u | vblank;
}
static uint32_t h_DisplayIsVblank(CpuState *s) {
    (void)s;
    return (sr_get_ge_status() & 0x20) ? 1 : 0;
}
/* sceDisplayGetFramePerSec returns the PSP display refresh rate as a
 * single-precision float through $f0 (the MIPS float-return convention); the
 * integer $v0 status is what the handler returns.  The value is the same
 * 60000/1001 rational the scheduler uses for its display phase, evaluated as
 * float == 0x426fc29f bits (59.9400599f, PSP-3001/6.61-ARK measured anchor).
 * The previous h_ok registration returned 0 in $v0 but left $f0 stale, so a
 * guest reading the float received poisoned state (issue #80 display-clock
 * campaign). */
static uint32_t h_DisplayGetFramePerSec(CpuState *s) {
    (void)s;
    s->f[0] = 60000.0f / 1001.0f;
    return 0;
}
void ge_set_frame(uint32_t frame);

void sr_vblank_tick(void) {
    if (s_display_latched_pending) {
        s_display_active = s_display_latched;
        s_display_latched_pending = 0;
        s_display_configured = 1;
        s_framebuf = s_display_active.addr;
        hle_watchdog_note_present();
        /* Same arm as the sync=0 immediate flip: a NEXTFRAME (sync=1) flip is presented
         * here, so the capture must be armed before this present or the recorded frame
         * is not the presented one. Arming only in the sync=0 branch left every
         * double-buffering title (which flips at VBLANK, not synchronously) with no
         * present-truthful capture at all. */
        fbcap_arm_for_present(s_vcount, &s_display_active, 0u, s_framebuf != 0);
        vramdump_try_present(s_vcount, &s_display_active);
        psmf_av_video_presented(s_display_active.addr, s_display_active.stride, s_display_active.fmt);
        display_present_active();
    }
    /* Periodic VCOUNT cadences are boundary crossings, not an exact modulus: a serviced
     * tick can step over a multiple or repeat one (see sr_vcount_window_crossed). */
    static uint32_t tick_log_window = 0;
    if (ge_log_on() && sr_vcount_window_crossed(s_vcount, 64u, &tick_log_window))
        fprintf(stderr, "VBLANK tick %u\n", s_vcount);
    vramdump_note_vblank(s_vcount);
    /* Full guest-PC dumps contain thousands of rows and synchronous five-second cadence
     * materially distorts the route being profiled. Keep periodic capture opt-in and let the
     * canonical manager choose a bounded default for runs that may be force-stopped before
     * atexit. Direct SR_PROFILE users otherwise get the normal exit dump only. */
    static uint32_t profile_dump_period = UINT32_MAX;
    if (profile_dump_period == UINT32_MAX) {
        const char *period = getenv("SR_PROFILE_DUMP_VBLANKS");
        unsigned long parsed = period && period[0] ? strtoul(period, NULL, 10) : 0;
        profile_dump_period = parsed > UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)parsed;
    }
    static uint32_t profile_dump_window = 0;
    if (profile_dump_period > 0 &&
        sr_vcount_window_crossed(s_vcount, profile_dump_period, &profile_dump_window)) {
        sr_profile_dump();
    }
    ge_set_frame(s_vcount);

    /* Issue #64: advance the state-qualified route before the controller sample is
     * latched, so a checkpoint observed on this vblank releases its press on this vblank
     * rather than one frame late. */
    route_tick(s_vcount_fwd);
    sr_ctrl_sample();   /* latch one controller sample per frame (PPSSPP ring semantics) */
    /* No-frame watchdog: vblanks keep being delivered even when every game thread is
     * blocked, so a stretch with no new sceDisplaySetFrameBuf is a NO-NEW-FLIP
     * observation. It is not by itself a hang, scheduler-stall, or scene-transition
     * verdict: a legitimately static scene (e.g. a save-confirmation modal waiting for
     * user input) also stops presenting. The display-outcome counters, thread wait
     * state, and movie-stack activity below are the facts that let a human classify
     * the stretch.
     * SR_WATCHDOG_EXIT=<N>: abort after N vblanks with no new frame (default: no abort). */
    uint32_t diff = s_vcount - s_last_flip_vcount;
    /* s_vcount is monotonic for the life of the process and s_last_flip_vcount
     * is only ever assigned from it, so this unsigned difference is a true
     * elapsed-period count (it cannot wrap short of 2^32 periods, ~2.2 years at
     * 60 Hz) and the bucket index below is monotonic within a stretch. */
    uint32_t bucket = diff / 600u;
    if (bucket > s_watchdog_bucket) {
        s_watchdog_bucket = bucket;
        s_watchdog_fires++;
        fprintf(stderr,
                "WATCHDOG: no new frame presented for %u vblanks (~%us) - neutral "
                "NO-NEW-FLIP observation, not by itself a hang/stall verdict\n",
                diff, diff / 60);
        uint32_t ready = 0u, delay = 0u, wait = 0u, spin = 0u;
        sched_watchdog_summary(&ready, &delay, &wait, &spin);
        fprintf(stderr,
                "BOOT_EVENT phase=stalled observation=no_new_flip "
                "no_frame_vblanks=%u seconds=%u ready=%u delay=%u wait=%u spin=%u\n",
                diff, diff / 60u, ready, delay, wait, spin);
        fflush(stderr);
        /* Which side of the display handoff stopped presenting: did the guest stop
         * asking for flips, or are its requests being refused? last_vcount vs
         * s_last_flip_vcount separates "no call since the last flip" from "calls
         * that did not present". */
        fprintf(stderr,
                "WATCHDOG_DISPLAY: calls=%lu immediate=%lu latched=%lu rejected=%lu "
                "last_call_v=%u last_req=0x%08x/%d/%d sync=%u "
                "last_err=0x%08x@v%u req=0x%08x/%d/%d sync=%u pending=%d active=0x%08x/%d/%d\n",
                s_setfb.calls, s_setfb.immediate, s_setfb.latched, s_setfb.rejected,
                s_setfb.last_vcount, s_setfb.last_addr, s_setfb.last_stride,
                s_setfb.last_fmt, s_setfb.last_sync,
                s_setfb.last_err, s_setfb.last_err_vcount, s_setfb.last_err_addr,
                s_setfb.last_err_stride, s_setfb.last_err_fmt, s_setfb.last_err_sync,
                s_display_latched_pending, s_display_active.addr,
                s_display_active.stride, s_display_active.fmt);
        /* Thread wait state: a parked thread's reason is read from the dump (wait
         * reason/deadline/callback/wakeup/join columns), not from a stale wait_obj. */
        sched_dump_threads();
        /* Movie-stack activity: sceMpeg and scePsmfPlayer are separate libraries; these
         * counters say whether either stack was being fed. */
        fprintf(stderr, "WATCHDOG_MPEG: put=%lu getavc=%lu avcdec=%lu nodata=%lu\n",
                g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata);
        fprintf(stderr, "WATCHDOG_PSMF: player_calls=%lu getvideo=%lu getaudio=%lu\n",
                s_psmf_calls, s_psmf_getvideo, s_psmf_getaudio);
        fflush(stderr);
        { static int wde = -1;
          if (wde < 0) { const char *e = getenv("SR_WATCHDOG_EXIT"); wde = e ? atoi(e) : 0; }
          if (wde > 0 && diff >= (uint32_t)wde) {
              fprintf(stderr, "WATCHDOG: aborting after %u vblanks with no new frame (SR_WATCHDOG_EXIT=%d)\n", diff, wde);
              sr_flight_hang(diff, (uint32_t)wde);
              sr_compat_report();
              sr_perf_shutdown();
              _Exit(1);
          }
        }
    }
    /* Dark-picture watch: flips keep arriving, but every probed frame since the stretch began
     * was dark. Like the no-frame watchdog, a neutral observation for the player and a
     * person to classify, not a hang verdict. */
    {
        uint32_t dark = 0u;
        /* A probe still being read back is collected first, without waiting; it is waited
         * for only when this tick's report could depend on its result. */
        picture_probe_collect(0);
        if (sr_picture_watch_tick_needs_probe(&s_picture_watch, s_vcount))
            picture_probe_collect(1);
        if (sr_picture_watch_tick(&s_picture_watch, s_vcount, &dark) == SR_PICTURE_WATCH_REPORT) {
            uint32_t ready = 0u, delay = 0u, wait = 0u, spin = 0u;
            sched_watchdog_summary(&ready, &delay, &wait, &spin);
            fprintf(stderr,
                    "BOOT_EVENT phase=stalled observation=dark_picture "
                    "dark_vblanks=%u seconds=%u ready=%u delay=%u wait=%u spin=%u\n",
                    dark, dark / 60u, ready, delay, wait, spin);
            fflush(stderr);
        }
    }
    /* Frame capture happens at sceDisplaySetFrameBuf (the instant a finished frame is presented),
     * which is the correct moment -- snapshotting here at an arbitrary vblank catches a buffer the
     * game has already begun clearing for the next frame. */

    /* SR_EXIT_AT_VBLANK=<V>: terminate cleanly once the guest has delivered V vblanks.
     *
     * A scripted route's useful work ends at its last press plus a settle window, but the only
     * stop control was -Duration, a wall-clock guess. Guessing low silently truncates the route
     * before its last inputs fire; guessing high burns minutes replaying a finished scene. This
     * makes the stop condition the same quantity the route is written in, so a route that ends
     * at vblank 41,200 stops there on every machine regardless of how fast it ran.
     *
     * PLACEMENT (exact, not "fully accounted"): this is the LAST statement of the tick, so
     * vblank V is complete in every respect this function is responsible for -- the frame
     * counter is advanced, ge_set_frame(V) has run, sr_ctrl_sample() has latched V's controller
     * sample (so a pad-script press scheduled for V is delivered before the exit), and the latch
     * assist and no-frame watchdog have run. What it does NOT wait for is work that happens outside
     * sr_vblank_tick: guest threads resumed by this vblank run after it returns, and a frame
     * whose sceDisplaySetFrameBuf lands later in vblank V is neither presented nor captured. So
     * a route's last input must be scheduled comfortably before V -- a settle window of a few
     * hundred vblanks -- and the last capture of interest must come from an earlier vblank.
     *
     * It ends the process; it never skips guest work, changes pacing, or touches rendering.
     * Exit status 0 -- reaching the requested vblank is success, unlike the watchdog abort. */
    hle_state_sample_vblank(s_vcount);
    hle_enhancement_speed_vblank(s_vcount);
    {
        static uint32_t exit_at = 0; static int exit_init = 0;
        if (!exit_init) {
            exit_init = 1;
            const char *e = getenv("SR_EXIT_AT_VBLANK");
            unsigned long v = e && e[0] ? strtoul(e, NULL, 10) : 0ul;
            exit_at = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
        }
        if (exit_at && s_vcount >= exit_at) {
            fprintf(stderr, "BOOT_EVENT phase=exit_at_vblank vblanks=%u (SR_EXIT_AT_VBLANK=%u)\n",
                    s_vcount, exit_at);
            hle_exit_audio_stats();
            /* _Exit skips atexit, so the SR_AUDIODUMP file is closed here whether or not
             * SR_AUDIOSTAT is set; after the stats call above it is a no-op. */
            hle_exit_close_audio_dump();
            sr_dump_calls();
            fflush(stderr);
            fflush(stdout);
            sr_flight_budget(s_vcount);
            /* _Exit skips the atexit hook that writes the SR_PERF_JSON summary and the
             * final CSV row, so the perf shutdown runs here, like the audio dump close
             * above; it is idempotent and a no-op without SR_PERF. The PSMF A/V summary is an
             * atexit report too, so it is printed here as well (a no-op unless enabled). */
            psmf_av_final("exit_at_vblank");
            sr_compat_report();
            sr_perf_shutdown();
            hle_state_sample_close();
            _Exit(0);
        }
    }
}
static uint32_t h_DisplayGetVcount(CpuState *s) {
    (void)s;
    sched_vtime_refresh();
    return s_vcount;
}
/* The PSP scans hCountPerVblank=286 lines per frame (PPSSPP Core/HW/Display.cpp). The
 * scheduler owns the rational 59.94-Hz phase, so these observations advance with
 * elapsed guest time and remain unchanged when a game polls them repeatedly. */
static uint32_t h_DisplayGetCurrentHcount(CpuState *s) {
    (void)s;
    return sched_display_current_hcount();
}
static uint32_t h_DisplayGetAccumulatedHcount(CpuState *s) {
    (void)s;
    return sched_display_accumulated_hcount();
}

/* sceDisplaySetMode: the requested mode and size are logged and the call succeeds. The
 * runtime replays no title display-driver bring-up: a title initializes its own display
 * state through its own code (#363). */
static uint32_t h_DisplaySetMode(CpuState *s) {
    uint32_t mode = s->r[4], width = s->r[5], height = s->r[6];
    fprintf(stderr, "DISPLAY_SET_MODE: mode=%u %ux%u (title %s)\n", mode, width, height,
            sr_title_config()->source_id);
    fflush(stderr);
    return 0; /* SCE_DISPLAY_SET_MODE_SUCCESS */
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only call-through to the production DisplaySetMode handler. */
uint32_t sr_hle_test_display_set_mode(CpuState *s) { return h_DisplaySetMode(s); }
#endif
#ifdef SR_HLE_THREAD_SELFTEST

/* See the forward declaration next to the counters themselves. */
void sr_display_test_flip_counts(unsigned long *calls, unsigned long *immediate,
                                 unsigned long *latched, unsigned long *rejected,
                                 uint32_t *last_err) {
    if (calls)     *calls     = s_setfb.calls;
    if (immediate) *immediate = s_setfb.immediate;
    if (latched)   *latched   = s_setfb.latched;
    if (rejected)  *rejected  = s_setfb.rejected;
    if (last_err)  *last_err  = s_setfb.last_err;
}
#endif /* SR_HLE_THREAD_SELFTEST */

/* The flip handler on its own: the media selftest registers just this display call so a
 * delivered picture reaches the production present path (issue #279). */
void hle_register_display_setframebuf_handler(void) {
    sr_hle_register(0x289d82fe, "sceDisplaySetFrameBuf", h_DisplaySetFrameBuf);
}

void hle_register_display_handlers(void) {
    hle_register_display_setframebuf_handler();
    sr_hle_register(0xeeda2e54, "sceDisplayGetFrameBuf", h_DisplayGetFrameBuf);
    /* 0x36cdfade is sceDisplayWaitVblank (issue #83); the CB variant is 0x46f186c3 and
     * remains unregistered until the callback-aware wait transaction lands. */
    sr_hle_register(0x0e20f177, "sceDisplaySetMode", h_DisplaySetMode);
    sr_hle_register(0x9c6eaad7, "sceDisplayGetVcount", h_DisplayGetVcount);
    sr_hle_register(0x773dd3a3, "sceDisplayGetCurrentHcount", h_DisplayGetCurrentHcount);
    sr_hle_register(0x210eab3a, "sceDisplayGetAccumulatedHcount", h_DisplayGetAccumulatedHcount);
    sr_hle_register(0x4d4e10ec, "sceDisplayIsVblank", h_DisplayIsVblank);
    sr_hle_register(0xdea197d4, "sceDisplayGetMode", h_DisplayGetMode);
    sr_hle_register(0xdba6c4c4, "sceDisplayGetFramePerSec", h_DisplayGetFramePerSec);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_display(void) {
    sr_hle_register(0x40f1469c, "sceDisplayWaitVblankStartMulti", h_DisplayWaitVblankStartMulti);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_display(void) {
    sr_hle_register_unsupported(0x7ed59bc4u, "sceDisplaySetHoldMode", 0x80020002u);
    sr_hle_register_unsupported(0x77ed8b3au, "sceDisplayWaitVblankStartMultiCB", 0x80020002u);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_display(void) {
    sr_hle_register(0x36cdfade, "sceDisplayWaitVblank", h_DisplayWaitVblank);
    sr_hle_register(0x984c27e7, "sceDisplayWaitVblankStart", h_DisplayWaitVblankStart);
    sr_hle_register(0x8eb9ec49, "sceDisplayWaitVblankCB", h_DisplayWaitVblankCB);
    sr_hle_register(0x46f186c3, "sceDisplayWaitVblankStartCB", h_DisplayWaitVblankStartCB);
}
