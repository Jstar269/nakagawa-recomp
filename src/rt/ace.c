/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace.c - Ace Mode's launch line, stamps, 30 Hz detector, speed readout and per-run report
 * (contract in ace.h). C library only.
 */
#include "ace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the launch line ------------------------------------------------------------------------ */

static void why_set(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
}

static int value_on_off(const char *v, int *out) {
    if (strcmp(v, "on") == 0) { *out = 1; return 1; }
    if (strcmp(v, "off") == 0) { *out = 0; return 1; }
    return 0;
}

int ace_parse(const char *spec, AceConfig *out, char *why, size_t why_cap) {
    static const char *const keys[] = { "af", "text", "readout", "notice", "wide", "smooth" };
    enum { KEYS = (int)(sizeof(keys) / sizeof(keys[0])) };
    AceConfig c;
    char buf[ACE_SPEC_MAX];
    int seen[KEYS] = {0};
    memset(&c, 0, sizeof(c));
    if (out) memset(out, 0, sizeof(*out));
    if (!spec) { why_set(why, why_cap, "unset"); return 0; }
    if (strcmp(spec, "off") == 0) { why_set(why, why_cap, ""); return 1; }
    if (strlen(spec) >= sizeof(buf)) { why_set(why, why_cap, "line too long"); return 0; }
    memcpy(buf, spec, strlen(spec) + 1u);

    char *field = buf;
    int first = 1;
    for (;;) {
        char *end = strchr(field, ';');
        if (end) *end = '\0';
        if (!field[0]) { why_set(why, why_cap, "empty field"); return 0; }
        if (first) {
            if (strcmp(field, "v1") != 0) { why_set(why, why_cap, "unknown version"); return 0; }
            first = 0;
        } else {
            char *eq = strchr(field, '=');
            int k = -1;
            if (!eq || eq == field || !eq[1]) { why_set(why, why_cap, "field is not key=value"); return 0; }
            *eq = '\0';
            const char *v = eq + 1;
            for (int i = 0; i < KEYS; i++)
                if (strcmp(field, keys[i]) == 0) k = i;
            if (k < 0) { why_set(why, why_cap, "unknown key"); return 0; }
            if (seen[k]) { why_set(why, why_cap, "duplicate key"); return 0; }
            seen[k] = 1;
            int ok = 0;
            switch (k) {
            case 0:
                ok = strcmp(v, "0") == 0 || strcmp(v, "2") == 0 || strcmp(v, "4") == 0 ||
                     strcmp(v, "8") == 0 || strcmp(v, "16") == 0;
                if (ok) c.af = atoi(v);
                break;
            case 1: ok = value_on_off(v, &c.text); break;
            case 2: ok = value_on_off(v, &c.readout); break;
            case 3: ok = value_on_off(v, &c.notice); break;
            case 4:
                if (strcmp(v, "off") == 0) { c.wide = ACE_WIDE_OFF; ok = 1; }
                else if (strcmp(v, "fill") == 0) { c.wide = ACE_WIDE_FILL; ok = 1; }
                if (ok && c.wide && !ACE_HAS_WIDE) {
                    why_set(why, why_cap, "wide is not part of this build"); return 0;
                }
                break;
            default:
                if (strcmp(v, "off") == 0) { c.smooth = ACE_SMOOTH_OFF; ok = 1; }
                else if (strcmp(v, "match") == 0) { c.smooth = ACE_SMOOTH_MATCH; ok = 1; }
                else if (strcmp(v, "x2") == 0) { c.smooth = ACE_SMOOTH_X2; ok = 1; }
                if (ok && c.smooth && !ACE_HAS_SMOOTH) {
                    why_set(why, why_cap, "smooth is not part of this build"); return 0;
                }
                break;
            }
            if (!ok) { why_set(why, why_cap, "bad value"); return 0; }
        }
        if (!end) break;
        field = end + 1;
    }
    for (int i = 0; i < KEYS; i++)
        if (!seen[i]) { why_set(why, why_cap, "missing key"); return 0; }
    c.on = 1;
    if (out) *out = c;
    why_set(why, why_cap, "");
    return 1;
}

static const char *on_off(int v) { return v ? "on" : "off"; }
static const char *wide_word(int v) { return v == ACE_WIDE_FILL ? "fill" : "off"; }
static const char *smooth_word(int v) {
    return v == ACE_SMOOTH_MATCH ? "match" : v == ACE_SMOOTH_X2 ? "x2" : "off";
}

int ace_format(const AceConfig *c, char *out, size_t cap) {
    int n;
    if (!out || !cap) return 0;
    if (!c || !c->on) n = snprintf(out, cap, "off");
    else n = snprintf(out, cap, "v1;af=%d;text=%s;readout=%s;notice=%s;wide=%s;smooth=%s",
                      c->af, on_off(c->text), on_off(c->readout), on_off(c->notice),
                      wide_word(c->wide), smooth_word(c->smooth));
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return 1;
}

/* ---- the run's switch ----------------------------------------------------------------------- */

int g_ace_on;

static AceConfig s_req, s_eff;
static char s_spec[ACE_SPEC_MAX] = "off";
static char s_report_path[1024];
static char s_unavailable[ACE_FEATURE_COUNT][96];
static int s_af_device;          /* the device's level when lower than the request, else 0 */

static const char *const k_feature_key[ACE_FEATURE_COUNT] = {
    "af", "text", "readout", "notice", "wide", "smooth"
};

/* The run's observation state. */
static AceDetector s_det;
static AceReadoutState s_readout;
static AceReadoutView s_view;
static int s_have_view;
static AceCounters s_counters;
static uint64_t s_win_start_ns;
static int s_win_started;
static uint32_t s_win_frames, s_win_skipped;
static uint64_t s_win_idle_ns;
static uint32_t s_last_flip_vcount, s_last_flip_fb;
static int s_have_flip;
static uint64_t s_rate_vblanks[ACE_RATE_COUNT];
static uint64_t s_limited_windows;

#define ACE_TARGETS 8
typedef struct { uint32_t fb, bytes, draws, seen, touched; int used; } AceTarget;
static AceTarget s_targets[ACE_TARGETS];
static uint32_t s_target_clock;

static void observation_reset(void) {
    ace_det_reset(&s_det);
    ace_readout_reset(&s_readout);
    memset(&s_view, 0, sizeof(s_view));
    s_have_view = 0;
    memset(&s_counters, 0, sizeof(s_counters));
    s_win_start_ns = 0; s_win_started = 0;
    s_win_frames = s_win_skipped = 0; s_win_idle_ns = 0;
    s_last_flip_vcount = s_last_flip_fb = 0; s_have_flip = 0;
    memset(s_rate_vblanks, 0, sizeof(s_rate_vblanks));
    s_limited_windows = 0;
    memset(s_targets, 0, sizeof(s_targets));
    s_target_clock = 0;
}

int ace_init_spec(const char *spec, const char *report_path) {
    char why[64];
    g_ace_on = 0;
    memset(&s_req, 0, sizeof(s_req));
    memset(&s_eff, 0, sizeof(s_eff));
    memset(s_unavailable, 0, sizeof(s_unavailable));
    s_af_device = 0;
    s_report_path[0] = '\0';
    snprintf(s_spec, sizeof(s_spec), "off");
    observation_reset();
    if (!spec) return 0;                       /* unset: off, silently */
    if (!ace_parse(spec, &s_req, why, sizeof(why))) {
        fprintf(stderr, "ACE_MODE: refused (%s)\n", why);
        return 0;
    }
    if (!s_req.on) return 0;                   /* "off": authentic, silently */
    s_eff = s_req;
    (void)ace_format(&s_req, s_spec, sizeof(s_spec));
    if (report_path && report_path[0])
        snprintf(s_report_path, sizeof(s_report_path), "%s", report_path);
    g_ace_on = 1;
    fprintf(stderr, "ACE_MODE: on (af=%d, text=%s, readout=%s, notice=%s, wide=%s, smooth=%s)\n",
            s_req.af, on_off(s_req.text), on_off(s_req.readout), on_off(s_req.notice),
            wide_word(s_req.wide), smooth_word(s_req.smooth));
    return 1;
}

int ace_init_from_env(void) {
    return ace_init_spec(getenv("SR_ACE"), getenv("SR_ACE_REPORT"));
}

const AceConfig *ace_requested(void) { return &s_req; }
const AceConfig *ace_effective(void) { return &s_eff; }
const char *ace_spec_text(void) { return s_spec; }

void ace_feature_unavailable(AceFeature feature, const char *reason) {
    if (!g_ace_on || (unsigned)feature >= ACE_FEATURE_COUNT) return;
    if (s_unavailable[feature][0]) return;     /* named once */
    snprintf(s_unavailable[feature], sizeof(s_unavailable[feature]), "%s",
             reason && reason[0] ? reason : "not available on this PC");
    switch (feature) {
    case ACE_FEATURE_AF: s_eff.af = 0; break;
    case ACE_FEATURE_TEXT: s_eff.text = 0; break;
    case ACE_FEATURE_READOUT: s_eff.readout = 0; break;
    case ACE_FEATURE_NOTICE: s_eff.notice = 0; break;
    case ACE_FEATURE_WIDE: s_eff.wide = 0; break;
    default: s_eff.smooth = 0; break;
    }
    fprintf(stderr, "ACE_MODE: %s unavailable (%s)\n", k_feature_key[feature], s_unavailable[feature]);
}

void ace_set_af_effective(int af, const char *reason) {
    if (!g_ace_on || !s_req.af) return;
    if (af <= 0) { ace_feature_unavailable(ACE_FEATURE_AF, reason); return; }
    if (af < s_req.af) {
        s_eff.af = af;
        s_af_device = af;
        fprintf(stderr, "ACE_MODE: af %d (the device's most)\n", af);
    }
}

static size_t words_add(char *out, size_t cap, size_t used, const char *word) {
    int n = snprintf(out + used, used < cap ? cap - used : 0, "%s%s",
                     used ? " \xC2\xB7 " : "", word);
    if (n < 0 || used + (size_t)n >= cap) return used;
    return used + (size_t)n;
}

size_t ace_feature_words(char *out, size_t cap) {
    size_t used = 0;
    char af[32];
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!g_ace_on) return 0;
    if (s_eff.af) {
        snprintf(af, sizeof(af), "%dx filtering", s_eff.af);
        used = words_add(out, cap, used, af);
    }
    if (s_eff.text) used = words_add(out, cap, used, "crisp keyboard");
    if (s_eff.wide) used = words_add(out, cap, used, "ultrawide");
    if (s_eff.smooth) used = words_add(out, cap, used, "smooth motion");
    if (s_eff.readout) used = words_add(out, cap, used, "speed readout");
    return used;
}

/* ---- the 30 Hz detector --------------------------------------------------------------------- */

void ace_det_reset(AceDetector *d) {
    if (d) memset(d, 0, sizeof(*d));
}

AceRate ace_det_window_class(const AceDetector *d) {
    if (!d || d->count < ACE_DET_WINDOW) return ACE_RATE_NONE;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < ACE_DET_WINDOW; i++) sum += d->interval[i];
    for (uint32_t k = 1; k <= 4; k++) {
        uint32_t lo = ACE_DET_WINDOW * k - 2u, hi = ACE_DET_WINDOW * k + 4u, near = 0;
        if (sum < lo || sum > hi) continue;
        for (uint32_t i = 0; i < ACE_DET_WINDOW; i++) {
            uint32_t v = d->interval[i];
            if (v + 1u >= k && v <= k + 1u) near++;
        }
        if (near >= ACE_DET_WINDOW - 4u) return (AceRate)k;
    }
    return ACE_RATE_VARIABLE;
}

const char *ace_rate_name(AceRate rate) {
    switch (rate) {
    case ACE_RATE_K1: return "60 Hz";
    case ACE_RATE_K2: return "30 Hz";
    case ACE_RATE_K3: return "20 Hz";
    case ACE_RATE_K4: return "15 Hz";
    case ACE_RATE_STATIC: return "static";
    case ACE_RATE_VARIABLE: return "variable";
    default: return "none";
    }
}

/* Moves the stable class toward `raw` under the hysteresis rule. Returns 1 when it changed. */
static int det_settle(AceDetector *d, AceRate raw, uint32_t vcount) {
    AceRate before = d->stable;
    if (raw == ACE_RATE_NONE) return 0;        /* window not full yet: nothing to judge */
    /* STATIC keeps the last class: frames that resume at that rate return to it at once. */
    if (d->stable == ACE_RATE_STATIC && raw == d->last_rate) {
        d->stable = raw;
        d->candidate = ACE_RATE_NONE;
        d->disagreeing = 0;
        return 1;
    }
    if (raw == d->stable) {
        d->disagreeing = 0;
        d->candidate = ACE_RATE_NONE;
        return 0;
    }
    /* STATIC is already ACE_DET_STATIC_VBLANKS of evidence: entered at once. */
    if (raw == ACE_RATE_STATIC) {
        d->stable = raw;
        d->candidate = ACE_RATE_NONE;
        d->disagreeing = 0;
        return 1;
    }
    if (raw != d->candidate) {
        d->candidate = raw;
        d->candidate_since = vcount;
    }
    if (d->stable != ACE_RATE_NONE) {
        if (!d->disagreeing) {
            d->disagreeing = 1;
            d->disagree_since = vcount;
        }
        if (vcount - d->disagree_since >= ACE_DET_LEAVE_VBLANKS) {
            d->stable = ACE_RATE_NONE;
            d->disagreeing = 0;
        }
    }
    /* Any other class needs 1.0 s of agreement. */
    if (d->stable == ACE_RATE_NONE && vcount - d->candidate_since >= ACE_DET_ENTER_VBLANKS) {
        d->stable = raw;
        d->candidate = ACE_RATE_NONE;
        d->disagreeing = 0;
    }
    if (d->stable >= ACE_RATE_K1 && d->stable <= ACE_RATE_K4) {
        d->last_rate = d->stable;
        d->ever_stable = 1;
    }
    return d->stable != before;
}

int ace_det_flip(AceDetector *d, uint32_t vcount, int new_frame) {
    if (!d) return 0;
    if (!new_frame) {
        if (d->have_last && vcount - d->last_new_vcount >= ACE_DET_STATIC_VBLANKS)
            return det_settle(d, ACE_RATE_STATIC, vcount);
        return 0;
    }
    if (d->have_last) {
        uint32_t gap = vcount - d->last_new_vcount;
        /* A gap of a static stretch or longer is not a frame interval: the window keeps the
         * rate the game had, and frames that resume are judged from here. */
        if (gap < ACE_DET_STATIC_VBLANKS) {
            d->interval[d->head] = (uint8_t)gap;
            d->head = (d->head + 1u) % ACE_DET_WINDOW;
            if (d->count < ACE_DET_WINDOW) d->count++;
        }
    }
    d->have_last = 1;
    d->last_new_vcount = vcount;
    return det_settle(d, ace_det_window_class(d), vcount);
}

/* ---- the speed readout ---------------------------------------------------------------------- */

#define ACE_VBLANK_HZ (60000.0 / 1001.0)

void ace_readout_reset(AceReadoutState *s) {
    if (s) memset(s, 0, sizeof(*s));
}

static int rate_k(AceRate r) {
    return r >= ACE_RATE_K1 && r <= ACE_RATE_K4 ? (int)r : 0;
}

void ace_readout_judge(AceReadoutState *s, const AceWindow *w, AceReadoutView *out) {
    AceReadoutView v;
    memset(&v, 0, sizeof(v));
    if (!s || !w || !out) return;
    double secs = w->wall_ns ? (double)w->wall_ns / 1e9 : 1.0;
    v.fps = (double)w->new_frames / secs;
    v.idle_share = w->wall_ns ? (double)w->idle_ns / (double)w->wall_ns : 0.0;
    if (v.idle_share > 1.0) v.idle_share = 1.0;
    /* The class frames are judged against: the stretch's while inside one, else the one now
     * stable, else the last stable one. */
    int k_now = rate_k(w->stable) ? rate_k(w->stable) : rate_k(w->last_rate);
    int k_ref = s->in_stretch && s->stretch_k ? s->stretch_k : k_now;
    int slow = k_ref && v.fps < (ACE_VBLANK_HZ / (double)k_ref) * 0.97;
    v.pc_limited = w->skipped_slots > 0 || (v.idle_share < 0.02 && slow);
    if (v.pc_limited && !s->in_stretch) {
        s->in_stretch = 1;
        s->stretch_k = w->ever_stable ? k_now : 0;
    } else if (!v.pc_limited) {
        s->in_stretch = 0;
        s->stretch_k = 0;
    }
    int fps_shown = (int)(v.fps + 0.5);
    if (!w->ever_stable || (v.pc_limited && !s->stretch_k)) {
        v.have_target = 0;
        v.speed_pct = 100;
        if (v.pc_limited) snprintf(v.text, sizeof(v.text), "\xE2\x96\xB2 \xE2\x80\x94  %d FPS  PC LIMITED", fps_shown);
        else snprintf(v.text, sizeof(v.text), "\xE2\x80\x94 %d FPS", fps_shown);
    } else if (v.pc_limited) {
        v.have_target = 1;
        v.target_fps = ACE_VBLANK_HZ / (double)s->stretch_k;
        double pct = 100.0 * v.fps / v.target_fps;
        v.speed_pct = pct >= 100.0 ? 100 : (int)pct;
        snprintf(v.text, sizeof(v.text), "\xE2\x96\xB2 %d%%  %d FPS  PC LIMITED", v.speed_pct, fps_shown);
    } else {
        v.have_target = 1;
        v.target_fps = k_now ? ACE_VBLANK_HZ / (double)k_now : 0.0;
        v.speed_pct = 100;
        snprintf(v.text, sizeof(v.text), "100%%  %d FPS", fps_shown);
    }
    *out = v;
}

/* ---- observation feed ----------------------------------------------------------------------- */

static AceTarget *target_slot(uint32_t fb, int create) {
    AceTarget *free_slot = NULL, *oldest = &s_targets[0];
    for (int i = 0; i < ACE_TARGETS; i++) {
        AceTarget *t = &s_targets[i];
        if (t->used && t->fb == fb) return t;
        if (!t->used && !free_slot) free_slot = t;
        if (t->touched < oldest->touched) oldest = t;
    }
    if (!create) return NULL;
    AceTarget *t = free_slot ? free_slot : oldest;
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->fb = fb;
    return t;
}

/* The slot for `fb`, marked as the most recently used: eviction takes the oldest. */
static AceTarget *target_touch(uint32_t fb) {
    AceTarget *t = target_slot(fb, 1);
    t->touched = ++s_target_clock;
    return t;
}

void ace_note_target_draw(uint32_t fb) {
    if (!g_ace_on) return;
    AceTarget *t = target_touch(fb);
    t->draws++;
    s_counters.draws++;
}

void ace_note_vram_write(uint32_t off, uint32_t bytes) {
    if (!g_ace_on || !bytes) return;
    uint64_t lo = off, hi = (uint64_t)off + bytes;
    for (int i = 0; i < ACE_TARGETS; i++) {
        AceTarget *t = &s_targets[i];
        if (!t->used || !t->bytes) continue;
        if (lo < (uint64_t)t->fb + t->bytes && t->fb < hi) {
            t->draws++;
            s_counters.draws++;
        }
    }
}

void ace_note_host_wait(uint64_t ns) {
    if (!g_ace_on) return;
    s_win_idle_ns += ns;
    s_counters.host_wait_ns += ns;
}

void ace_note_skipped_slots(uint32_t slots) {
    if (!g_ace_on || !slots) return;
    s_win_skipped += slots;
    s_counters.skipped_slots += slots;
}

int ace_flip(uint32_t fb, uint32_t bytes, uint32_t vcount, uint64_t now_ns) {
    if (!g_ace_on) return 0;
    AceTarget *t = target_touch(fb);
    t->bytes = bytes;
    /* "seen" holds the draw count at the target's previous flip; the high bit marks a target
     * flipped at least once, so its first flip is new. */
    int new_frame = (t->draws & 0x7FFFFFFFu) != (t->seen & 0x7FFFFFFFu) ||
                    !(t->seen & 0x80000000u) || (s_have_flip && fb != s_last_flip_fb);
    t->seen = (t->draws & 0x7FFFFFFFu) | 0x80000000u;
    if (s_have_flip) s_rate_vblanks[s_det.stable] += (uint64_t)(vcount - s_last_flip_vcount);
    s_last_flip_vcount = vcount;
    s_last_flip_fb = fb;
    s_have_flip = 1;
    s_counters.flips++;
    if (new_frame) { s_counters.new_frames++; s_win_frames++; }
    if (ace_det_flip(&s_det, vcount, new_frame))
        fprintf(stderr, "ACE_RATE: %s\n", ace_rate_name(s_det.stable));
    if (!s_win_started) {
        s_win_started = 1;
        s_win_start_ns = now_ns;
        s_win_frames = 0; s_win_skipped = 0; s_win_idle_ns = 0;
    } else if (now_ns - s_win_start_ns >= 1000000000ull) {
        AceWindow w;
        w.wall_ns = now_ns - s_win_start_ns;
        w.new_frames = s_win_frames;
        w.idle_ns = s_win_idle_ns;
        w.skipped_slots = s_win_skipped;
        w.stable = s_det.stable;
        w.ever_stable = s_det.ever_stable;
        w.last_rate = s_det.last_rate;
        ace_readout_judge(&s_readout, &w, &s_view);
        s_have_view = 1;
        s_counters.windows++;
        if (s_view.pc_limited) s_limited_windows++;
        s_win_start_ns = now_ns;
        s_win_frames = 0; s_win_skipped = 0; s_win_idle_ns = 0;
    }
    return new_frame;
}

int ace_readout_view(AceReadoutView *out) {
    if (!g_ace_on || !s_have_view) return 0;
    if (out) *out = s_view;
    return 1;
}

AceRate ace_rate_stable(void) { return g_ace_on ? s_det.stable : ACE_RATE_NONE; }

void ace_counters(AceCounters *out) {
    if (out) *out = s_counters;
}

/* ---- the per-run report -------------------------------------------------------------------- */

static AceRate dominant_rate(void) {
    AceRate best = ACE_RATE_NONE;
    uint64_t most = 0;
    for (int r = ACE_RATE_K1; r < ACE_RATE_COUNT; r++) {
        if (s_rate_vblanks[r] > most) { most = s_rate_vblanks[r]; best = (AceRate)r; }
    }
    return best;
}

size_t ace_report_json(char *out, size_t cap) {
    size_t used = 0;
    int n;
#define ACE_PUT(...) do { \
        n = snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__); \
        if (n < 0 || used + (size_t)n >= cap) { if (cap) out[0] = '\0'; return 0; } \
        used += (size_t)n; \
    } while (0)
    if (!out || !cap) return 0;
    ACE_PUT("{\n  \"schema\": 1,\n  \"spec\": \"%s\",\n", s_spec);
    ACE_PUT("  \"effective\": {\"af\": %d, \"text\": %s, \"readout\": %s, \"notice\": %s, "
            "\"wide\": \"%s\", \"smooth\": \"%s\"},\n",
            s_eff.af, s_eff.text ? "true" : "false", s_eff.readout ? "true" : "false",
            s_eff.notice ? "true" : "false", wide_word(s_eff.wide), smooth_word(s_eff.smooth));
    ACE_PUT("  \"unavailable\": {");
    int first = 1;
    for (int f = 0; f < ACE_FEATURE_COUNT; f++) {
        if (!s_unavailable[f][0]) continue;
        /* Reasons are runtime-authored words; quotes and backslashes are dropped. */
        char clean[96];
        size_t j = 0;
        for (size_t i = 0; s_unavailable[f][i] && j + 1 < sizeof(clean); i++) {
            char ch = s_unavailable[f][i];
            if (ch != '"' && ch != '\\' && (unsigned char)ch >= 0x20) clean[j++] = ch;
        }
        clean[j] = '\0';
        ACE_PUT("%s\"%s\": \"%s\"", first ? "" : ", ", k_feature_key[f], clean);
        first = 0;
    }
    ACE_PUT("},\n");
    if (s_af_device) ACE_PUT("  \"af_device_max\": %d,\n", s_af_device);
    ACE_PUT("  \"rate\": {\"dominant\": \"%s\", \"seconds\": {", ace_rate_name(dominant_rate()));
    first = 1;
    for (int r = ACE_RATE_K1; r < ACE_RATE_COUNT; r++) {
        if (!s_rate_vblanks[r]) continue;
        ACE_PUT("%s\"%s\": %.1f", first ? "" : ", ", ace_rate_name((AceRate)r),
                (double)s_rate_vblanks[r] / ACE_VBLANK_HZ);
        first = 0;
    }
    ACE_PUT("}},\n");
    ACE_PUT("  \"frames\": {\"flips\": %llu, \"new\": %llu},\n",
            (unsigned long long)s_counters.flips, (unsigned long long)s_counters.new_frames);
    ACE_PUT("  \"pc_limited_seconds\": %llu\n}\n", (unsigned long long)s_limited_windows);
#undef ACE_PUT
    return used;
}

void ace_finish(void) {
    char json[ACE_REPORT_MAX];
    if (!g_ace_on) return;
    size_t len = ace_report_json(json, sizeof(json));
    fprintf(stderr, "ACE_MODE: exit (%s; rate %s)\n", s_spec, ace_rate_name(dominant_rate()));
    if (!len || !s_report_path[0]) return;
    char tmp[sizeof(s_report_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_report_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "ACE_MODE: report not written (%s)\n", "cannot create the file");
        return;
    }
    int ok = fwrite(json, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    /* rename() does not replace an existing file on Windows: drop the old report first. A
     * crash in between leaves no report, which the launcher reads as "no last session". */
    if (ok) {
        (void)remove(s_report_path);
        ok = rename(tmp, s_report_path) == 0;
    }
    if (!ok) {
        (void)remove(tmp);
        fprintf(stderr, "ACE_MODE: report not written (%s)\n", "write failed");
    }
}
