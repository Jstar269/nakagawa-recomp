// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Acceptance routes and route observation (#64), and present-truthful frame capture (#57).
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"
#include "nid_names.h"   /* sr_nid_name(): names unknown NIDs in the trap below */

#define ROUTE_MAX_CP     32
#define ROUTE_MAX_ALT    4
#define ROUTE_MIN_ACTIVE_BYTES 12
#define ROUTE_MAX_STEPS  256
/* A bare pad script is one line per press, so a timed sweep script runs to hundreds of lines
 * (the v0.0.1 sweep's has 453); past this many the file is refused, never cut short. */
#define ROUTE_MAX_LEGACY 4096
#define ROUTE_NAME_MAX   32
#define ROUTE_FAIL_EXIT  86

enum { ROUTE_OP_WAIT = 1, ROUTE_OP_EXPECT, ROUTE_OP_PRESS, ROUTE_OP_DELAY, ROUTE_OP_UNTIL,
       ROUTE_OP_WHILE, ROUTE_OP_NID, ROUTE_OP_UNTIL_NID, ROUTE_OP_END };
enum { ROUTE_OFF = 0, ROUTE_LEGACY, ROUTE_RUNNING, ROUTE_DONE, ROUTE_FAILED };

/* One named screen. Parts of a screen legitimately vary between otherwise identical
 * replays -- HST redraws its menus over a club backdrop that is not the same every run --
 * so a whole-frame comparison rejects the right screen. Recording the same screen twice
 * under different variable content solves this without a hand-authored region mask: bytes
 * that disagree between the alternates carry the variation, not the identity, and are
 * dropped from the comparison. Measured on the two menu backdrops seen here that leaves
 * about half the frame informative, and the Main Menu still sits ~8x closer to itself than
 * to the submenu it was being confused with. A checkpoint recorded once compares whole. */
typedef struct {
    char    name[ROUTE_NAME_MAX];
    int     tol;
    int     nsig;
    int     nactive;
    uint8_t sig[ROUTE_MAX_ALT][ROUTE_SIG_MAX];
    uint8_t active[ROUTE_SIG_MAX];   /* 1 = this byte takes part in the comparison */
} RouteCheckpoint;

typedef struct {
    int      op;
    char     name[ROUTE_NAME_MAX];   /* WAIT / EXPECT / PRESS_UNTIL */
    uint32_t a, b, c, d;             /* PRESS: mask,width   DELAY/WAIT: vblanks
                                      * PRESS_UNTIL: mask,width,period,timeout */
    int      line;                   /* source line, for diagnostics */
    int      reads;                  /* the widths above count guest reads, not vblanks
                                      * (WIDTHS READS, or a READS line); the timeouts never do */
    uint32_t nid;                    /* PRESS_UNTIL_NID: the import whose call completes the step */
} RouteStep;

static RouteCheckpoint s_route_cp[ROUTE_MAX_CP];
static int      s_route_ncp;
static RouteStep s_route_prog[ROUTE_MAX_STEPS];
static int      s_route_nsteps;
static int      s_route_pc;
static uint32_t s_route_step_start;
static int      s_route_step_started;
static int      s_route_state = ROUTE_OFF;
static int      s_route_tol = 12;
static int      s_route_sample_every = 20;
static uint32_t s_route_keys;
/* Elapsed-cadence bookkeeping for the route observer (#109): the delivered
 * VCOUNT of the last due sampling attempt and whether one has happened since
 * the route (re)started. Reset by sr_route_reset and sr_route_load so a fresh
 * route always gets its first pending observation immediately. */
static uint32_t s_route_last_attempt;
static int      s_route_have_attempt;
static struct { uint32_t f, mask, w; } s_route_legacy[ROUTE_MAX_LEGACY];
static int      s_route_nlegacy;
static int      s_route_loaded;
/* WIDTHS VBLANKS|READS: the unit the widths of a route program's PRESS, DELAY, PRESS_UNTIL and
 * PRESS_WHILE steps count in when the step does not say otherwise. Vblanks until a file says
 * WIDTHS READS, so a route written before the keyword existed is exactly the route it was. */
static int      s_route_widths_reads;
static int      s_route_widths_set;
static uint32_t       s_input_budget = SR_INPUT_DEFAULT_READ_BUDGET;
/* The legacy table as the segment list that reproduces it (2 * rows + 1 always suffices). */
static SrInputSegment s_route_legacy_seg[ROUTE_MAX_LEGACY * 2 + 1];
static int            s_route_legacy_nseg;
static int            s_route_legacy_pos;
static int            s_route_legacy_reported;   /* the "delivered" line has been printed */
/* The auto-START pulse's schedule (see auto_pulse_keys): the configuration it was armed
 * with and the nominal VCOUNT of its next press. */
static struct {
    int      armed;
    uint32_t mask, period, width, start;
    uint32_t next;
} s_pulse;
static void auto_pulse_disarm(void) { s_pulse.armed = 0; }

/* SR_PADSCRIPT_READ_BUDGET=<vblanks>: how long a scripted press may wait for the guest to
 * read it before the run fails. Unset or empty keeps the default; anything that is not a
 * whole vblank count >= 1 is refused rather than read as some other budget. */
static int input_read_budget_from_env(uint32_t *out) {
    const char *e = getenv("SR_PADSCRIPT_READ_BUDGET");
    *out = SR_INPUT_DEFAULT_READ_BUDGET;
    if (!e || !e[0]) return 0;
    char *end = NULL;
    unsigned long v = strtoul(e, &end, 10);
    if (!end || *end || e[0] == '-' || v < 1ul || v > UINT32_MAX) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* WAIT_NID: gate a step on a guest-visible EVENT rather than on what a screen looks like.
 *
 * A screen signature has to be recorded from a run that is already on that screen, and until
 * a route can reach a screen it cannot record one: the step before any checkpoint is the one
 * that needs a screen nobody has a signature for. What the guest does, on the other hand, is
 * observable from the first boot -- a module load, a savedata status poll, a display mode
 * change -- and it is the same for every title, so a route file can name it and the runtime
 * stays title-neutral.
 *
 * sr_syscall() is the single point every guest import passes through, so the observation is
 * one store there and one compare here. The ring holds the most recent dispatches; a step
 * matches only an entry newer than the moment the step began, so an event that already
 * happened cannot satisfy a later step. Set when the running step is a WAIT_NID, so a build
 * with no route pays one predictable branch per import. PRESS_UNTIL_NID does not read the ring
 * (see s_route_until_seen). */
#define ROUTE_NID_RING 64
static uint32_t s_route_nid_ring[ROUTE_NID_RING];
static unsigned long s_route_nid_pos;      /* total dispatches observed, ever */
int      s_route_watch_nid;         /* a running WAIT_NID step wants the ring */
static unsigned long s_route_nid_start;    /* dispatch count when that step began */

/* PRESS_UNTIL_NID: the import a running step presses until, and whether the guest has called it
 * since that step was armed. The ring is WAIT_NID's window, and a burst of more than ROUTE_NID_RING
 * imports between two route ticks would push an awaited call out of it, so this step keeps its own
 * flag. route_note_nid sets it; that runs only while the NID watch is on, and a PRESS_UNTIL_NID step
 * turns the watch on when it arms, so the two agree. Guest threads are coroutines on the scheduler's
 * host thread (sr_coro.h), so the import hook and the route tick never run at once and the flag needs
 * no lock, as the ring does not. */
static int      s_route_until_armed;       /* a running PRESS_UNTIL_NID step is waiting */
static uint32_t s_route_until_nid;         /* the import that step presses until */
static int      s_route_until_seen;        /* that import was called since the step was armed */

/* The one place the ring is written, so the dispatcher's gate and the selftest's feed are
 * the same code. The PRESS_UNTIL_NID flag is set here for the same reason. */
void route_note_nid(uint32_t nid) {
    s_route_nid_ring[s_route_nid_pos % ROUTE_NID_RING] = nid;
    s_route_nid_pos++;
    if (s_route_until_armed && nid == s_route_until_nid) s_route_until_seen = 1;
}

/* Has the guest called `nid` since the running step began? */
static int route_nid_since(uint32_t nid) {
    unsigned long seen = s_route_nid_pos - s_route_nid_start;
    if (seen > ROUTE_NID_RING) seen = ROUTE_NID_RING;
    for (unsigned long i = 0; i < seen; i++) {
        unsigned long idx = s_route_nid_pos - 1 - i;
        if (s_route_nid_ring[idx % ROUTE_NID_RING] == nid) return 1;
    }
    return 0;
}

static void route_nid_watch(int on) {
    s_route_watch_nid = on;
    s_route_nid_start = s_route_nid_pos;
}

/* Arm the PRESS_UNTIL_NID flag for the step that is starting (`on` set, `nid` its import), or
 * clear it when a step begins or ends without one. Arming resets the flag, so a call made before
 * the step began, or during an earlier step, never counts. */
static void route_until_nid_arm(int on, uint32_t nid) {
    s_route_until_armed = on;
    s_route_until_nid = nid;
    s_route_until_seen = 0;
}

/* The SR_AUDIOSTAT statistics that both _Exit paths print before they finalise the SR_AUDIODUMP
 * file: the ATRAC and SAS counters, the buffer identities, then the host backend line. The
 * SR_EXIT_AT_VBLANK exit and route_fail share this block, so a failed route reports the same audio
 * statistics a clean vblank-budget exit reports (#827). Nothing prints without SR_AUDIOSTAT. */
void hle_exit_audio_stats(void) {
    if (!audio_stat_on()) return;
    fprintf(stderr,
            "AUDIOSTAT_ATRAC: frames=%lu nonzero=%lu peak=%d\n",
            g_atrac_frames, g_atrac_frames_nonzero, g_atrac_peak);
    fprintf(stderr,
            "AUDIOSTAT_SAS: calls=%lu withmix=%lu pre_nonzero=%lu post_nonzero=%lu erased=%lu pre_peak=%d post_peak=%d no_voice=%lu no_voice_overwrite=%lu\n",
            g_sas_calls, g_sas_calls_add, g_sas_pre_nonzero,
            g_sas_post_nonzero, g_sas_erased, g_sas_pre_peak, g_sas_post_peak,
            g_sas_no_voice, g_sas_no_voice_overwrite);
    fprintf(stderr, "AUDIOSTAT_BUF: atrac_out n=%d", g_atrac_out_nbufs);
    for (int i = 0; i < g_atrac_out_nbufs; i++)
        fprintf(stderr, " 0x%08x", g_atrac_out_bufs[i]);
    fprintf(stderr, "\nAUDIOSTAT_BUF: sas_out n=%d", g_sas_nbufs);
    for (int i = 0; i < g_sas_nbufs; i++)
        fprintf(stderr, " 0x%08x", g_sas_bufs[i]);
    fprintf(stderr, "\n");
    for (uint32_t c = 0; c < 9u; c++) {
        if (!g_audio_nbufs[c]) continue;
        fprintf(stderr, "AUDIOSTAT_BUF: ch=%u n=%d", c, g_audio_nbufs[c]);
        for (int i = 0; i < g_audio_nbufs[c]; i++)
            fprintf(stderr, " 0x%08x", g_audio_bufs[c][i]);
        fprintf(stderr, "\n");
    }
#ifndef SR_HLE_THREAD_SELFTEST
    /* audio.c is not linked into the executable HLE harness. */
    sr_audio_dump_stats();
#endif
}

/* _Exit skips atexit, so a hard exit closes the SR_AUDIODUMP file itself, which finalises
 * its WAV header. The SR_EXIT_AT_VBLANK exit and route_fail both call this outside any
 * SR_AUDIOSTAT gate; a second close is a no-op. Only the public audio backend exports it. */
void hle_exit_close_audio_dump(void) {
#if !defined(SR_HLE_THREAD_SELFTEST) && defined(SR_PUBLIC_SAFE)
    sr_audio_dump_finish();
#endif
}

/* A route that cannot be trusted must not be allowed to look like one that can. Every
 * failure path lands here: it names the step, the vblank and what was actually on screen,
 * then terminates with a distinct status so the manager's verdict cannot record the run
 * as complete. Tests set SR_ROUTE_NO_EXIT to inspect the terminal state instead. */
static void route_fail(const char *fmt, ...) {
    va_list ap;
    s_route_state = ROUTE_FAILED;
    route_until_nid_arm(0, 0u);
    fputs("ROUTE_FAIL: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (!getenv("SR_ROUTE_NO_EXIT")) {
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, 0u, ROUTE_FAIL_EXIT);
        hle_exit_audio_stats();
        hle_exit_close_audio_dump();
        sr_compat_report();
        sr_perf_shutdown();
        _Exit(ROUTE_FAIL_EXIT);
    }
}

static int route_hex_nib(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The pad bits a route can name, in the order they are listed in the layout the guest
 * reads (NK_PSP_BTN_*_BIT, shared with both host front-ends). A route that presses the
 * wrong bit does not fail: the game receives a button the screen ignores and the run
 * looks exactly like a frozen game, which is the most expensive way to learn that a
 * mask was one bit out. Naming the button removes the counting, and an unknown name is
 * refused rather than read as a mask that happens to parse. */
static const struct { const char *name; uint32_t bit; } s_route_btn[] = {
    { "SELECT",   NK_PSP_BTN_SELECT_BIT   },
    { "START",    NK_PSP_BTN_START_BIT    },
    { "UP",       NK_PSP_BTN_UP_BIT       },
    { "RIGHT",    NK_PSP_BTN_RIGHT_BIT    },
    { "DOWN",     NK_PSP_BTN_DOWN_BIT     },
    { "LEFT",     NK_PSP_BTN_LEFT_BIT     },
    { "L",        NK_PSP_BTN_LTRIGGER_BIT },
    { "R",        NK_PSP_BTN_RTRIGGER_BIT },
    { "TRIANGLE", NK_PSP_BTN_TRIANGLE_BIT },
    { "CIRCLE",   NK_PSP_BTN_CIRCLE_BIT   },
    { "CROSS",    NK_PSP_BTN_CROSS_BIT    },
    { "SQUARE",   NK_PSP_BTN_SQUARE_BIT   },
    { "HOME",     NK_PSP_BTN_HOME_BIT     },
    { "HOLD",     NK_PSP_BTN_HOLD_BIT     },
};
#define ROUTE_NBTN ((int)(sizeof s_route_btn / sizeof s_route_btn[0]))

static int route_casecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb || ca == 0) return ca - cb;
    }
}

/* A press mask is either a hex literal (as every route written before this existed, with
 * or without the 0x prefix the legacy table accepts) or one or more button names joined by
 * '+'. A token is read as hex only when every one of its characters is a hex digit, because
 * several names begin with one (CROSS, CIRCLE) and a prefix test would swallow them whole.
 * The hex form is bounded: at most 8 digits and at least one, because `strtoul` reports
 * neither a value too wide for the mask nor a bare "0x", and both would arrive as a
 * different mask than the route asked for.
 * Returns 0 on success, -1 for anything else, so an unreadable token -- an unknown button
 * included -- fails the route at load instead of pressing nothing for the rest of the run.
 * "The press never arrived" is indistinguishable from a hang from outside
 * (docs/DEBUGGING.md, "A press that arrives and is ignored"). */
static int route_parse_mask(const char *tok, uint32_t *out) {
    if (!tok || !tok[0]) return -1;
    const char *digits = (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) ? tok + 2 : tok;
    int all_hex = digits[0] != 0;
    for (const char *p = digits; *p; p++)
        if (route_hex_nib((unsigned char)*p) < 0) { all_hex = 0; break; }
    if (all_hex) {
        int n = 0;
        uint32_t v = 0;
        for (const char *p = digits; *p; p++, n++) {
            if (n >= 8) return -1;              /* wider than the mask: refuse, do not truncate */
            v = (v << 4) | (uint32_t)route_hex_nib((unsigned char)*p);
        }
        if (n == 0) return -1;                  /* a bare "0x" names no button */
        *out = v;
        return 0;
    }
    uint32_t mask = 0;
    const char *p = tok;
    for (;;) {
        char name[16];
        int n = 0;
        while (p[n] && p[n] != '+' && n < (int)sizeof name - 1) { name[n] = p[n]; n++; }
        name[n] = 0;
        if (n == 0 || (size_t)n >= sizeof name) return -1;
        int found = -1;
        for (int i = 0; i < ROUTE_NBTN; i++)
            if (route_casecmp(name, s_route_btn[i].name) == 0) { found = i; break; }
        if (found < 0) return -1;
        mask |= s_route_btn[found].bit;
        if (p[n] == 0) break;
        p += n + 1;                     /* skip the '+' */
    }
    *out = mask;
    return 0;
}

/* An import named as the runtime names it, or as raw hex. The hex form exists so a route
 * never depends on a name being in the table: the NID is the guest-visible fact, the name is
 * a convenience. */
static int route_nid_from_token(const char *tok, uint32_t *out) {
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) return route_parse_mask(tok, out);
    for (size_t i = 0; i < sr_nid_table_count; i++)
        if (strcmp(sr_nid_table[i].name, tok) == 0) { *out = sr_nid_table[i].nid; return 0; }
    return -1;
}

/* What a mask presses, in words. Printed once per press step at load so any run's own
 * log says which buttons the route asked for: "the route pressed START and the screen
 * ignored it" is a one-line diagnosis, while the same fact spread across a hex mask is
 * a puzzle. */
static void route_describe_mask(uint32_t mask, char *out, size_t n) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < ROUTE_NBTN; i++) {
        if (!(mask & s_route_btn[i].bit)) continue;
        int w = snprintf(out + used, n - used, "%s%s", used ? "+" : "", s_route_btn[i].name);
        if (w < 0 || (size_t)w >= n - used) return;
        used += (size_t)w;
    }
    if (used == 0 && n > 8) snprintf(out, n, "0x%04x", mask);
}

/* The names a mask argument may use, for the refusal that has to teach the fix. */
static const char *route_button_names(void) {
    return "SELECT START UP RIGHT DOWN LEFT L R TRIANGLE CIRCLE CROSS SQUARE HOME HOLD, "
           "joinable with '+'";
}

/* Exactly `want` bytes of hex and nothing else: a truncated signature would silently
 * compare only its prefix and match screens it has never seen. */
static int route_parse_sig(const char *hex, uint8_t *out, int want) {
    int n = 0;
    while (hex[0] && hex[1] && n < want) {
        int hi = route_hex_nib((unsigned char)hex[0]);
        int lo = route_hex_nib((unsigned char)hex[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return (n == want && *hex == '\0') ? 0 : -1;
}

/* Mean absolute difference over the checkpoint's informative bytes, taking the closest
 * recorded alternate. */
static int route_distance(const RouteCheckpoint *cp, const uint8_t *sig) {
    int n = route_sig_bytes(), best = 255;
    if (cp->nactive <= 0) return 255;
    for (int k = 0; k < cp->nsig; k++) {
        long sum = 0;
        for (int i = 0; i < n; i++) {
            if (!cp->active[i]) continue;
            int d = (int)cp->sig[k][i] - (int)sig[i];
            sum += d < 0 ? -d : d;
        }
        int mean = (int)(sum / cp->nactive);
        if (mean < best) best = mean;
    }
    return best;
}

/* Closest defined checkpoint to what is on screen. A failure that can say "this looks
 * like SINGLE_PLAYER_MENU (d=3)" is a diagnosis; "not MAIN_MENU" is only a complaint. */
static int route_best_match(const uint8_t *sig, int *out_d) {
    int best = -1, best_d = 255;
    for (int i = 0; i < s_route_ncp; i++) {
        int d = route_distance(&s_route_cp[i], sig);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (out_d) *out_d = best_d;
    return best;
}

/* What the run last actually saw. A failure diagnosed only from the vblank it fired on is
 * usually blank -- observations are periodic and a timeout rarely lands on one -- so the
 * last observation is kept and named instead. */
static char s_route_seen[96] = "no screen was observed at all";
static int  s_route_while_seen;   /* PRESS_WHILE has seen its screen at least once */

static const char *route_seen_desc(void) { return s_route_seen; }

static int route_find(const char *name) {
    for (int i = 0; i < s_route_ncp; i++)
        if (strcmp(s_route_cp[i].name, name) == 0) return i;
    return -1;
}

static int route_matches(const char *name, const uint8_t *sig, int *out_d) {
    int i = route_find(name);
    if (i < 0) { if (out_d) *out_d = 255; return 0; }
    int d = route_distance(&s_route_cp[i], sig);
    if (out_d) *out_d = d;
    return d <= s_route_cp[i].tol;
}

/* Decide which bytes of a checkpoint carry its identity. With one recorded signature that
 * is the whole frame; with several it is the bytes they agree on. */
static int route_finalize_checkpoints(const char *path) {
    int n = route_sig_bytes();
    for (int c = 0; c < s_route_ncp; c++) {
        RouteCheckpoint *cp = &s_route_cp[c];
        cp->nactive = 0;
        for (int i = 0; i < n; i++) {
            int lo = 255, hi = 0;
            for (int k = 0; k < cp->nsig; k++) {
                int v = cp->sig[k][i];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            cp->active[i] = (uint8_t)(hi - lo <= cp->tol);
            cp->nactive += cp->active[i];
        }
        if (cp->nactive < ROUTE_MIN_ACTIVE_BYTES) {
            fprintf(stderr,
                    "ROUTE_PARSE: %s: checkpoint '%s' has only %d of %d bytes in common across "
                    "its %d recorded signatures; there is not enough left to identify a screen\n",
                    path, cp->name, cp->nactive, n, cp->nsig);
            return -1;
        }
        if (cp->nsig > 1)
            fprintf(stderr, "ROUTE: checkpoint %s uses %d of %d bytes (%d signatures, tol=%d)\n",
                    cp->name, cp->nactive, n, cp->nsig, cp->tol);
    }
    /* The real requirement is not how much of a frame survives masking -- a title screen is
     * mostly backdrop and legitimately keeps a fifth of it -- but that the checkpoints can
     * still be told apart. Two that match each other make every assertion between them
     * vacuous, and a route that cannot fail is worse than no route, so refuse it here rather
     * than let it report success. */
    for (int a = 0; a < s_route_ncp; a++)
        for (int b = 0; b < s_route_ncp; b++) {
            if (a == b) continue;
            for (int k = 0; k < s_route_cp[b].nsig; k++) {
                int d = route_distance(&s_route_cp[a], s_route_cp[b].sig[k]);
                if (d <= s_route_cp[a].tol) {
                    fprintf(stderr,
                            "ROUTE_PARSE: %s: checkpoint '%s' also matches a recording of '%s' "
                            "(d=%d, tol=%d); the route could not tell them apart\n",
                            path, s_route_cp[a].name, s_route_cp[b].name, d, s_route_cp[a].tol);
                    return -1;
                }
            }
        }
    return 0;
}

/* Route files are authored by hand beside the private run inputs, so every parse error is
 * reported with its line and refuses the route rather than running a partial program. */
static int route_parse_line(char *line, int lineno, const char *path) {
    char *tok = strtok(line, " \t\r\n");
    if (!tok || tok[0] == '#') return 0;

    if (tok[0] >= '0' && tok[0] <= '9') {
        if (s_route_nsteps > 0 || s_route_ncp > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: bare frame line inside a route program\n", path, lineno);
            return -1;
        }
        if (s_route_nlegacy >= ROUTE_MAX_LEGACY) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d pad-script lines\n",
                    path, lineno, ROUTE_MAX_LEGACY);
            return -1;
        }
        char *m = strtok(NULL, " \t\r\n");
        char *w = strtok(NULL, " \t\r\n");
        if (!m || !w) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: expected '<frame> <hexmask> <width>'\n", path, lineno);
            return -1;
        }
        uint32_t lmask = 0;
        if (route_parse_mask(m, &lmask) != 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: '%s' is not a hex mask or a button name (%s)\n",
                    path, lineno, m, route_button_names());
            return -1;
        }
        s_route_legacy[s_route_nlegacy].f    = (uint32_t)strtoul(tok, NULL, 10);
        s_route_legacy[s_route_nlegacy].mask = lmask;
        s_route_legacy[s_route_nlegacy].w    = (uint32_t)strtoul(w, NULL, 10);
        s_route_nlegacy++;
        return 0;
    }

    if (s_route_nlegacy > 0) {
        fprintf(stderr, "ROUTE_PARSE: %s:%d: route program mixed with bare frame lines\n", path, lineno);
        return -1;
    }

    /* WIDTHS names the unit for the whole file, so it must come before any step it would apply
     * to, and only once: a second setting would silently change what the earlier steps meant. */
    if (strcmp(tok, "WIDTHS") == 0) {
        char *unit = strtok(NULL, " \t\r\n");
        if (!unit || (strcmp(unit, "VBLANKS") != 0 && strcmp(unit, "READS") != 0)) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: WIDTHS VBLANKS or WIDTHS READS\n", path, lineno);
            return -1;
        }
        if (s_route_widths_set) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: WIDTHS may be given once\n", path, lineno);
            return -1;
        }
        if (s_route_nsteps > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: WIDTHS must precede every step\n", path, lineno);
            return -1;
        }
        s_route_widths_set = 1;
        s_route_widths_reads = unit[0] == 'R';
        return 0;
    }

    /* READS and VBLANKS come first on a line and state the unit of that one step. Existing
     * lines never begin with either word, so they parse exactly as before. Only the four
     * steps whose widths can differ accept one; anything else would ignore the unit silently. */
    int line_reads = s_route_widths_reads;
    if (strcmp(tok, "READS") == 0 || strcmp(tok, "VBLANKS") == 0) {
        line_reads = tok[0] == 'R';
        tok = strtok(NULL, " \t\r\n");
        if (!tok || tok[0] == '#') {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: READS and VBLANKS must be followed by PRESS, DELAY, "
                            "PRESS_UNTIL or PRESS_WHILE\n", path, lineno);
            return -1;
        }
        if (strcmp(tok, "PRESS") != 0 && strcmp(tok, "DELAY") != 0 &&
            strcmp(tok, "PRESS_UNTIL") != 0 && strcmp(tok, "PRESS_WHILE") != 0 &&
            strcmp(tok, "PRESS_UNTIL_NID") != 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: READS and VBLANKS apply to PRESS, DELAY, PRESS_UNTIL "
                            "and PRESS_WHILE, and PRESS_UNTIL_NID, not '%s'\n", path, lineno, tok);
            return -1;
        }
    }

    if (strcmp(tok, "SIGGRID") == 0) {
        char *c = strtok(NULL, " \t\r\n"), *r = strtok(NULL, " \t\r\n");
        if (!c || !r) { fprintf(stderr, "ROUTE_PARSE: %s:%d: SIGGRID <cols> <rows>\n", path, lineno); return -1; }
        int cols = atoi(c), rows = atoi(r);
        if (cols < 1 || rows < 1 || cols * rows > ROUTE_MAX_CELLS) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: SIGGRID %dx%d out of range (cols*rows <= %d)\n",
                    path, lineno, cols, rows, ROUTE_MAX_CELLS);
            return -1;
        }
        if (s_route_ncp > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: SIGGRID must precede every CHECKPOINT\n", path, lineno);
            return -1;
        }
        s_route_cols = cols; s_route_rows = rows;
        return 0;
    }
    if (strcmp(tok, "SAMPLE_EVERY") == 0 || strcmp(tok, "TOLERANCE") == 0) {
        char *v = strtok(NULL, " \t\r\n");
        int n = v ? atoi(v) : -1;
        if (n < 1) { fprintf(stderr, "ROUTE_PARSE: %s:%d: %s <n>, n >= 1\n", path, lineno, tok); return -1; }
        if (tok[0] == 'S') { s_route_sample_every = n; return 0; }
        /* A checkpoint takes the tolerance in force when it is parsed, and its tolerance
         * also decides which bytes are informative. A later TOLERANCE would silently mean
         * something different for the checkpoints above it than the file appears to say. */
        if (s_route_ncp > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: TOLERANCE must precede every CHECKPOINT\n", path, lineno);
            return -1;
        }
        s_route_tol = n;
        return 0;
    }
    if (strcmp(tok, "CHECKPOINT") == 0) {
        char *name = strtok(NULL, " \t\r\n");
        char *next = name ? strtok(NULL, " \t\r\n") : NULL;
        int tol = s_route_tol;
        if (!name || !next) { fprintf(stderr, "ROUTE_PARSE: %s:%d: CHECKPOINT <NAME> [tol=<n>] <hex>\n", path, lineno); return -1; }
        if (strncmp(next, "tol=", 4) == 0) {
            tol = atoi(next + 4);
            next = strtok(NULL, " \t\r\n");
            if (tol < 0 || !next) { fprintf(stderr, "ROUTE_PARSE: %s:%d: bad tol= or missing signature\n", path, lineno); return -1; }
        }
        if (strlen(name) >= ROUTE_NAME_MAX) { fprintf(stderr, "ROUTE_PARSE: %s:%d: checkpoint name too long\n", path, lineno); return -1; }
        int idx = route_find(name);
        if (idx < 0) {
            if (s_route_ncp >= ROUTE_MAX_CP) { fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d checkpoints\n", path, lineno, ROUTE_MAX_CP); return -1; }
            idx = s_route_ncp++;
            memset(&s_route_cp[idx], 0, sizeof s_route_cp[idx]);
            snprintf(s_route_cp[idx].name, ROUTE_NAME_MAX, "%s", name);
        }
        if (s_route_cp[idx].nsig >= ROUTE_MAX_ALT) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d signatures for '%s'\n", path, lineno, ROUTE_MAX_ALT, name);
            return -1;
        }
        if (route_parse_sig(next, s_route_cp[idx].sig[s_route_cp[idx].nsig], route_sig_bytes()) != 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: signature must be exactly %d hex bytes for a %dx%d grid\n",
                    path, lineno, route_sig_bytes(), s_route_cols, s_route_rows);
            return -1;
        }
        s_route_cp[idx].nsig++;
        s_route_cp[idx].tol = tol;
        return 0;
    }

    {
        RouteStep st;
        memset(&st, 0, sizeof st);
        st.line = lineno;
        st.reads = line_reads;
        if (strcmp(tok, "WAIT") == 0 || strcmp(tok, "EXPECT") == 0) {
            st.op = tok[0] == 'W' ? ROUTE_OP_WAIT : ROUTE_OP_EXPECT;
            char *name = strtok(NULL, " \t\r\n");
            if (!name || strlen(name) >= ROUTE_NAME_MAX) { fprintf(stderr, "ROUTE_PARSE: %s:%d: %s <NAME>\n", path, lineno, tok); return -1; }
            snprintf(st.name, ROUTE_NAME_MAX, "%s", name);
            if (st.op == ROUTE_OP_WAIT) {
                char *t = strtok(NULL, " \t\r\n");
                long to = t ? atol(t) : -1;
                if (to < 1) { fprintf(stderr, "ROUTE_PARSE: %s:%d: WAIT <NAME> <timeout_vblanks>\n", path, lineno); return -1; }
                st.a = (uint32_t)to;
            }
        } else if (strcmp(tok, "PRESS_UNTIL") == 0 || strcmp(tok, "PRESS_WHILE") == 0) {
            /* The boot prefix is the one part of a route that genuinely has to repeat an
             * input until something happens: the warning screens and the intro movie each
             * need their own START and there is no way to know in advance how many. Written
             * as a fixed table it is the worst kind of timed route -- every extra press is
             * one that lands on whatever comes next if the run is faster than the recording.
             * Written as a repeat-until-observed it stops the moment the screen arrives.
             *
             * PRESS_WHILE is the other half of that problem. Some screens accept an input
             * only once the work behind them finishes -- the title screen draws its NEW
             * GAME / CONTINUE options long before it will act on one, and no pixel says
             * which. Repeating until the *next* screen appears leaves a window in which a
             * press can still be latched by it; repeating only while the current screen is
             * on show ends the input well before anything else can receive it. */
            st.op = tok[6] == 'U' ? ROUTE_OP_UNTIL : ROUTE_OP_WHILE;
            char *name = strtok(NULL, " \t\r\n");
            char *m = strtok(NULL, " \t\r\n"), *w = strtok(NULL, " \t\r\n");
            char *p = strtok(NULL, " \t\r\n"), *t = strtok(NULL, " \t\r\n");
            if (!name || strlen(name) >= ROUTE_NAME_MAX || !m || !w || !p || !t) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: %s <NAME> <hexmask> <width> <period> <timeout>\n", path, lineno, tok);
                return -1;
            }
            snprintf(st.name, ROUTE_NAME_MAX, "%s", name);
            if (route_parse_mask(m, &st.a) != 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: %s: '%s' is not a hex mask or a button name "
                                "(%s)\n", path, lineno, tok, m, route_button_names());
                return -1;
            }
            st.b = (uint32_t)strtoul(w, NULL, 10);
            st.c = (uint32_t)strtoul(p, NULL, 10);
            st.d = (uint32_t)strtoul(t, NULL, 10);
            if (st.reads) {
                /* The width and the period are reads; the timeout is still vblanks, so the
                 * one comparison that would mix the two units is not made. */
                if (st.b < 1 || st.c <= st.b || st.d < 1) {
                    fprintf(stderr, "ROUTE_PARSE: %s:%d: %s with READS needs width >= 1, period > width "
                                    "(both in reads), timeout >= 1 vblank\n", path, lineno, tok);
                    return -1;
                }
            } else if (st.b < 1 || st.c <= st.b || st.d < st.c) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: %s needs width >= 1, period > width, timeout >= period\n", path, lineno, tok);
                return -1;
            }
        } else if (strcmp(tok, "PRESS_UNTIL_NID") == 0) {
            /* Repeat the press until the guest calls an import. PRESS_UNTIL repeats until a
             * screen is observed, and a screen can only be recorded from a run that has already
             * reached it. Some prompts (the savedata message boxes, the Yes/No that follows
             * them) do not change the screen when a press is taken, so no screen says that the
             * press landed. The import the guest calls next does say so, and it is the same in
             * every title. */
            st.op = ROUTE_OP_UNTIL_NID;
            char *nid = strtok(NULL, " \t\r\n");
            char *m = strtok(NULL, " \t\r\n"), *w = strtok(NULL, " \t\r\n");
            char *p = strtok(NULL, " \t\r\n"), *t = strtok(NULL, " \t\r\n");
            if (!nid || !m || !w || !p || !t) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS_UNTIL_NID <import|0xNID> <hexmask> <width> "
                                "<period> <timeout>\n", path, lineno);
                return -1;
            }
            if (route_nid_from_token(nid, &st.nid) != 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS_UNTIL_NID: '%s' is not an import name and not "
                                "a 0x-prefixed NID (the runtime's own table is src/rt/nid_names.h; a name "
                                "it does not carry can be written as hex)\n", path, lineno, nid);
                return -1;
            }
            if (route_parse_mask(m, &st.a) != 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS_UNTIL_NID: '%s' is not a hex mask or a button name "
                                "(%s)\n", path, lineno, m, route_button_names());
                return -1;
            }
            st.b = (uint32_t)strtoul(w, NULL, 10);
            st.c = (uint32_t)strtoul(p, NULL, 10);
            st.d = (uint32_t)strtoul(t, NULL, 10);
            if (st.reads) {
                if (st.b < 1 || st.c <= st.b || st.d < 1) {
                    fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS_UNTIL_NID with READS needs width >= 1, period > "
                                    "width (both in reads), timeout >= 1 vblank\n", path, lineno);
                    return -1;
                }
            } else if (st.b < 1 || st.c <= st.b || st.d < st.c) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS_UNTIL_NID needs width >= 1, period > width, "
                                "timeout >= period\n", path, lineno);
                return -1;
            }
        } else if (strcmp(tok, "PRESS") == 0) {
            st.op = ROUTE_OP_PRESS;
            char *m = strtok(NULL, " \t\r\n"), *w = strtok(NULL, " \t\r\n");
            if (!m || !w) { fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS <hexmask> <width>\n", path, lineno); return -1; }
            if (route_parse_mask(m, &st.a) != 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS: '%s' is not a hex mask or a button name (%s)\n",
                        path, lineno, m, route_button_names());
                return -1;
            }
            st.b = (uint32_t)strtoul(w, NULL, 10);
            if (st.b < 1) { fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS width must be >= 1\n", path, lineno); return -1; }
        } else if (strcmp(tok, "WAIT_NID") == 0) {
            st.op = ROUTE_OP_NID;
            char *nid = strtok(NULL, " \t\r\n"), *to = strtok(NULL, " \t\r\n");
            if (!nid || !to) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: WAIT_NID <import|0xNID> <timeout>\n", path, lineno);
                return -1;
            }
            if (route_nid_from_token(nid, &st.b) != 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: WAIT_NID: '%s' is not an import name and not "
                                "a 0x-prefixed NID (the runtime's own table is "
                                "src/rt/nid_names.h; a name it does not carry can be written as hex)\n",
                        path, lineno, nid);
                return -1;
            }
            st.a = (uint32_t)strtoul(to, NULL, 10);
            if (st.a < 1u) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: WAIT_NID timeout must be >= 1 vblank\n", path, lineno);
                return -1;
            }
        } else if (strcmp(tok, "DELAY") == 0) {
            st.op = ROUTE_OP_DELAY;
            char *n = strtok(NULL, " \t\r\n");
            long v = n ? atol(n) : -1;
            if (v < 0) { fprintf(stderr, "ROUTE_PARSE: %s:%d: DELAY <vblanks>\n", path, lineno); return -1; }
            st.a = (uint32_t)v;
        } else if (strcmp(tok, "END") == 0) {
            st.op = ROUTE_OP_END;
        } else {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: unknown route keyword '%s'\n", path, lineno, tok);
            return -1;
        }
        if (s_route_nsteps >= ROUTE_MAX_STEPS) { fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d steps\n", path, lineno, ROUTE_MAX_STEPS); return -1; }
        s_route_prog[s_route_nsteps++] = st;
    }
    return 0;
}

/* Discard every loaded route. Only the executable regression tests call this; a run loads
 * its route once. */
void sr_route_reset(void) {
    s_route_ncp = s_route_nsteps = s_route_nlegacy = 0;
    s_route_pc = 0;
    s_route_step_start = 0;
    s_route_step_started = 0;
    s_route_state = ROUTE_OFF;
    s_route_cols = 12; s_route_rows = 8;
    s_route_tol = 12;
    s_route_sample_every = 20;
    s_route_widths_reads = 0;
    s_route_widths_set = 0;
    s_route_keys = 0;
    s_route_while_seen = 0;
    s_route_last_attempt = 0;
    s_route_have_attempt = 0;
    route_nid_watch(0);
    route_until_nid_arm(0, 0u);
    snprintf(s_route_seen, sizeof s_route_seen, "no screen was observed at all");
    s_route_loaded = 0;
    sr_input_reset(&s_input);
    s_route_legacy_nseg = s_route_legacy_pos = s_route_legacy_reported = 0;
    s_input_budget = SR_INPUT_DEFAULT_READ_BUDGET;
    auto_pulse_disarm();
}

int sr_route_load(const char *path) {
    char line[4096];
    int lineno = 0, bad = 0;
    FILE *fp;

    s_route_ncp = s_route_nsteps = s_route_nlegacy = 0;
    s_route_pc = 0;
    s_route_step_started = 0;
    route_until_nid_arm(0, 0u);
    s_route_keys = 0;
    s_route_while_seen = 0;
    s_route_last_attempt = 0;
    s_route_have_attempt = 0;
    s_route_widths_reads = 0;
    s_route_widths_set = 0;
    s_route_state = ROUTE_OFF;
    s_route_loaded = 1;
    sr_input_reset(&s_input);
    s_route_legacy_nseg = s_route_legacy_pos = s_route_legacy_reported = 0;
    if (!path || !path[0]) return 0;
    if (input_read_budget_from_env(&s_input_budget) != 0) {
        route_fail("SR_PADSCRIPT_READ_BUDGET='%s' is not a vblank count >= 1; refusing to run "
                   "a pad script with an unknown read budget", getenv("SR_PADSCRIPT_READ_BUDGET"));
        return 0;
    }
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ROUTE_PARSE: cannot open route file '%s'\n", path);
        return 0;
    }
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        if (route_parse_line(line, lineno, path) != 0) { bad = 1; break; }
    }
    fclose(fp);
    if (bad) {
        route_fail("route file '%s' is not usable; refusing to run an unverified route", path);
        return 0;
    }
    if (s_route_nsteps > 0) {
        for (int i = 0; i < s_route_nsteps; i++) {
            RouteStep *st = &s_route_prog[i];
            if ((st->op == ROUTE_OP_WAIT || st->op == ROUTE_OP_EXPECT) && route_find(st->name) < 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: no CHECKPOINT defines '%s'\n", path, st->line, st->name);
                route_fail("route file '%s' names undefined checkpoints", path);
                return 0;
            }
        }
        if (route_finalize_checkpoints(path) != 0) {
            route_fail("route file '%s' has checkpoints that cannot be told apart", path);
            return 0;
        }
        s_route_state = ROUTE_RUNNING;
        fprintf(stderr, "ROUTE: program loaded from %s (%d steps, %d checkpoints, grid %dx%d, "
                        "sample_every=%d, tolerance=%d)\n",
                path, s_route_nsteps, s_route_ncp, s_route_cols, s_route_rows,
                s_route_sample_every, s_route_tol);
        /* Say what each press asks for, in button names. The guest is given the same bits
         * a player produces, so "the route pressed START and the title ignored it" is a
         * fact this log can state outright instead of leaving a reader to decode a mask. */
        for (int i = 0; i < s_route_nsteps; i++) {
            RouteStep *st = &s_route_prog[i];
            if (st->op != ROUTE_OP_PRESS && st->op != ROUTE_OP_UNTIL && st->op != ROUTE_OP_WHILE)
                continue;
            char what[64];
            route_describe_mask(st->a, what, sizeof what);
            fprintf(stderr, "ROUTE: step %d (%s) presses %s%s\n", i,
                    st->op == ROUTE_OP_PRESS ? "PRESS" :
                    st->op == ROUTE_OP_UNTIL ? "PRESS_UNTIL" : "PRESS_WHILE", what,
                    st->reads ? " (widths in guest reads)" : "");
        }
        /* And say what each WAIT_NID is waiting for, the same way: a route that stalls on an
         * event names the event in the log instead of leaving a reader to guess. */
        for (int i = 0; i < s_route_nsteps; i++) {
            RouteStep *st = &s_route_prog[i];
            if (st->op != ROUTE_OP_NID && st->op != ROUTE_OP_UNTIL_NID) continue;
            uint32_t nid = st->op == ROUTE_OP_NID ? st->b : st->nid;
            uint32_t to = st->op == ROUTE_OP_NID ? st->a : st->d;
            const char *nm = sr_nid_name(nid);
            fprintf(stderr, "ROUTE: step %d (%s) %s %s (0x%08x) for %u vblanks\n",
                    i, st->op == ROUTE_OP_NID ? "WAIT_NID" : "PRESS_UNTIL_NID",
                    st->op == ROUTE_OP_NID ? "waits for" : "presses until the guest calls",
                    nm ? nm : "an unnamed import", nid, to);
        }
        return 1;
    }
    if (s_route_nlegacy > 0) {
        uint32_t rf[ROUTE_MAX_LEGACY], rm[ROUTE_MAX_LEGACY], rw[ROUTE_MAX_LEGACY];
        for (int i = 0; i < s_route_nlegacy; i++) {
            rf[i] = s_route_legacy[i].f;
            rm[i] = s_route_legacy[i].mask;
            rw[i] = s_route_legacy[i].w;
        }
        s_route_legacy_nseg = sr_input_from_rows(
            rf, rm, rw, s_route_nlegacy, s_input_budget, s_route_legacy_seg,
            (int)(sizeof s_route_legacy_seg / sizeof s_route_legacy_seg[0]));
        if (s_route_legacy_nseg < 0) {
            s_route_legacy_nseg = 0;
            route_fail("pad script '%s' does not fit the segment table", path);
            return 0;
        }
        s_route_legacy_pos = 0;
        s_route_state = ROUTE_LEGACY;
        fprintf(stderr, "ROUTE: pad script loaded from %s (%d rows as %d segments, delivered in "
                        "guest time; a press may wait %u vblanks for the guest to read it)\n",
                path, s_route_nlegacy, s_route_legacy_nseg, s_input_budget);
        return 1;
    }
    if (s_route_ncp > 0) {
        /* Checkpoints with no program is an unfinished route file. Running it would leave
         * the pad on its default START pulse, which is not what the file asks for. */
        route_fail("route file '%s' defines checkpoints but no steps", path);
        return 0;
    }
    return 0;
}

/* A scripted press the guest never read. Before guest-time delivery the press simply ended
 * on schedule and the run carried on as if it had been made; now the run fails here, naming
 * the buttons, how long they were held and what the step was. */
static void route_input_overdue(const char *what_step, uint32_t v) {
    char what[64];
    route_describe_mask(s_input.seg.mask, what, sizeof what);
    if (s_input.seg.reads) {
        route_fail("%s: the guest read the controller only %u of the %u reads %s was held for, "
                   "within its budget (from vblank %u to %u); a read-width press is held until the "
                   "guest has read it that many times, for at most SR_PADSCRIPT_READ_BUDGET=%u vblanks",
                   what_step, s_input.reads, s_input.seg.reads, what, s_input.since, v,
                   s_input.seg.budget);
    } else {
        route_fail("%s: the guest did not read the controller while %s was held for %u vblanks "
                   "(from vblank %u to %u); a scripted press is held until the guest reads it, for "
                   "at most SR_PADSCRIPT_READ_BUDGET=%u vblanks",
                   what_step, what, v - s_input.since, s_input.since, v, s_input.seg.budget);
    }
    sr_input_idle(&s_input);
}

/* The segment a step starts: `mask` held (or released, when mask is 0) for `width` of the
 * step's unit. A vblank width is controller samples. A read width is guest reads, and the
 * sample count is left at its minimum, because a read-width segment is delivered by its reads. */
static SrInputSegment route_segment(uint32_t mask, uint32_t width, int reads, uint32_t not_before,
                                    uint32_t budget) {
    SrInputSegment s;
    s.mask = mask;
    s.samples = reads ? 1u : width;
    s.not_before = not_before;
    s.budget = budget;
    s.reads = reads ? width : 0u;
    return s;
}

/* One vblank of a legacy pad script: advance its segment list in guest time and return the
 * pad state for this vblank's sample. */
static uint32_t route_legacy_tick(uint32_t v) {
    int r = sr_input_list_tick(&s_input, s_route_legacy_seg, s_route_legacy_nseg,
                               &s_route_legacy_pos, v);
    if (r < 0) {
        char step[64];
        snprintf(step, sizeof step, "pad script segment %d of %d", s_route_legacy_pos,
                 s_route_legacy_nseg);
        route_input_overdue(step, v);
        return 0u;
    }
    if (r == 1 && !s_route_legacy_reported) {
        s_route_legacy_reported = 1;
        fprintf(stderr, "ROUTE: pad script delivered (%d segments) by vblank %u\n",
                s_route_legacy_nseg, v);
    }
    return sr_input_mask(&s_input);
}

static void route_load_once(void) {
    if (s_route_loaded) return;
    s_route_loaded = 1;
    s_route_learn = getenv("SR_ROUTE_LEARN") ? 1 : 0;
    const char *sp = getenv("SR_PADSCRIPT");
    if (sp && sp[0]) sr_route_load(sp);
}

/* A finished step disarms its PRESS_UNTIL_NID flag, so its call cannot complete the next step. */
static void route_advance(void) {
    s_route_pc++;
    s_route_step_started = 0;
    route_until_nid_arm(0, 0u);
}

/* The repeated press of PRESS_UNTIL / PRESS_WHILE in guest time: `width` samples pressed
 * until the guest has read it, then released for the rest of the `period` until the guest
 * has read the release, and again. These pulses carry no read budget of their own: the
 * step's screen test ends them, and the step's own timeout bounds a guest that never
 * reads (the boot prefix they exist for can legitimately ignore the pad for a while). */
static uint32_t route_pulse(const RouteStep *st, uint32_t v) {
    if (sr_input_delivered(&s_input)) {
        SrInputSegment next = s_input.seg.mask
            ? route_segment(0u, st->c - st->b, st->reads, v, 0u)
            : route_segment(st->a, st->b, st->reads, v, 0u);
        sr_input_start(&s_input, &next, v);
    }
    return sr_input_mask(&s_input);
}

/* One vblank of the route program. `sig` is the observed screen signature, or NULL when
 * no observation was taken this vblank. Returns the button mask for this vblank. */
uint32_t sr_route_step(uint32_t v, const uint8_t *sig) {
    uint32_t keys = 0;
    if (s_route_state != ROUTE_RUNNING) return 0;
    if (sig) {
        int bd = 255, bi = route_best_match(sig, &bd);
        if (bi >= 0 && bd <= s_route_cp[bi].tol)
            snprintf(s_route_seen, sizeof s_route_seen, "last saw %s at vblank %u (d=%d)",
                     s_route_cp[bi].name, v, bd);
        else
            snprintf(s_route_seen, sizeof s_route_seen,
                     "last saw an unrecorded screen at vblank %u (closest %s d=%d)", v,
                     bi >= 0 ? s_route_cp[bi].name : "-", bd);
    }
    for (int guard = 0; guard <= ROUTE_MAX_STEPS; guard++) {
        if (s_route_pc >= s_route_nsteps) {
            s_route_state = ROUTE_DONE;
            fprintf(stderr, "ROUTE_OK: %d steps completed by vblank %u\n", s_route_nsteps, v);
            return keys;
        }
        RouteStep *st = &s_route_prog[s_route_pc];
        if (!s_route_step_started) {
            s_route_step_start = v;
            s_route_step_started = 1;
            /* The NID watch is armed by the step that wants it, so "called since this step
             * began" is measured from this vblank and not from the start of the route. */
            route_nid_watch(st->op == ROUTE_OP_NID || st->op == ROUTE_OP_UNTIL_NID);
            route_until_nid_arm(st->op == ROUTE_OP_UNTIL_NID, st->nid);
            /* Input belongs to the step that makes it: a pressing step starts its first
             * segment here, every other step leaves the pad released. */
            if (st->op == ROUTE_OP_PRESS || st->op == ROUTE_OP_UNTIL || st->op == ROUTE_OP_WHILE ||
                st->op == ROUTE_OP_UNTIL_NID) {
                SrInputSegment press = route_segment(st->a, st->b, st->reads, v,
                                                     st->op == ROUTE_OP_PRESS ? s_input_budget : 0u);
                sr_input_start(&s_input, &press, v);
            } else if (st->op == ROUTE_OP_DELAY && st->a > 0u) {
                SrInputSegment gap = route_segment(0u, st->a, st->reads, v, 0u);
                sr_input_start(&s_input, &gap, v);
            } else {
                sr_input_idle(&s_input);
            }
        }
        uint32_t el = v - s_route_step_start;
        switch (st->op) {
        case ROUTE_OP_PRESS:
            /* Held for `width` samples and until the guest has read it (scripted_input.h). */
            if (sr_input_overdue(&s_input, v)) {
                char step[32];
                snprintf(step, sizeof step, "line %d: PRESS", st->line);
                route_input_overdue(step, v);
                return keys;
            }
            if (!sr_input_delivered(&s_input)) return keys | st->a;
            route_advance();
            continue;
        case ROUTE_OP_DELAY:
            /* Released for `n` samples and until the guest has read the release, so the
             * press before it and the press after it reach the guest as two presses. */
            if (st->a > 0u && !sr_input_delivered(&s_input)) return keys;
            route_advance();
            continue;
        case ROUTE_OP_UNTIL: {
            int d = 255;
            if (sig && route_matches(st->name, sig, &d)) {
                fprintf(stderr, "ROUTE: reached %s at vblank %u (step %d, d=%d, after %u vblanks)\n",
                        st->name, v, s_route_pc, d, el);
                route_advance();
                continue;
            }
            if (el >= st->d) {
                route_fail("line %d: PRESS_UNTIL %s gave up after %u vblanks (from vblank %u); %s",
                           st->line, st->name, el, s_route_step_start, route_seen_desc());
                return keys;
            }
            return keys | route_pulse(st, v);
        }
        case ROUTE_OP_UNTIL_NID: {
            /* The press repeats until the guest calls the import, measured from the vblank the
             * step began; an import that happened before then does not count (the flag is armed
             * with the step, and route_note_nid sets it only for a call made after that). */
            const char *nm = sr_nid_name(st->nid);
            if (s_route_until_seen) {
                fprintf(stderr, "ROUTE: guest called %s (0x%08x) at vblank %u (step %d, after %u "
                                "vblanks; the press repeated until then)\n",
                        nm ? nm : "an unnamed import", st->nid, v, s_route_pc, el);
                route_advance();
                continue;
            }
            if (el >= st->d) {
                route_fail("line %d: PRESS_UNTIL_NID %s (0x%08x) was not called within %u vblanks "
                           "(from vblank %u to %u); the guest made %lu imports in that time, "
                           "none of them this one",
                           st->line, nm ? nm : "?", st->nid, el, s_route_step_start, v,
                           s_route_nid_pos - s_route_nid_start);
                return keys;
            }
            return keys | route_pulse(st, v);
        }
        case ROUTE_OP_WHILE: {
            int d = 255;
            if (sig && !route_matches(st->name, sig, &d)) {
                if (s_route_while_seen) {
                    fprintf(stderr, "ROUTE: left %s at vblank %u (step %d, after %u vblanks)\n",
                            st->name, v, s_route_pc, el);
                    s_route_while_seen = 0;
                    route_advance();
                    continue;
                }
            } else if (sig) {
                /* Do not let the step complete before its screen was ever on show: entering
                 * it one vblank early would otherwise skip the input entirely. */
                s_route_while_seen = 1;
            }
            if (el >= st->d) {
                int ever = s_route_while_seen;
                s_route_while_seen = 0;
                route_fail("line %d: PRESS_WHILE %s gave up after %u vblanks (from vblank %u); "
                           "the screen %s; %s",
                           st->line, st->name, el, s_route_step_start,
                           ever ? "never went away" : "was never on show", route_seen_desc());
                return keys;
            }
            return keys | route_pulse(st, v);
        }
        case ROUTE_OP_WAIT: {
            int d = 255;
            if (sig && route_matches(st->name, sig, &d)) {
                fprintf(stderr, "ROUTE: reached %s at vblank %u (step %d, d=%d)\n",
                        st->name, v, s_route_pc, d);
                route_advance();
                continue;
            }
            if (el >= st->a) {
                int wi = route_find(st->name);
                route_fail("line %d: WAIT %s timed out after %u vblanks (from vblank %u to %u); "
                           "%s (a match needs d<=%d)",
                           st->line, st->name, el, s_route_step_start, v, route_seen_desc(),
                           wi >= 0 ? s_route_cp[wi].tol : s_route_tol);
                return keys;
            }
            return keys;
        }
        case ROUTE_OP_EXPECT: {
            if (!sig) {
                /* An EXPECT that never receives an observation must not stall forever: the
                 * framebuffer sync can fail, and a silent stall would look like progress. */
                if (el >= (uint32_t)(s_route_sample_every * 8 + 60)) {
                    route_fail("line %d: EXPECT %s had no framebuffer observation for %u vblanks "
                               "from vblank %u", st->line, st->name, el, s_route_step_start);
                    return keys;
                }
                return keys;
            }
            int d = 255;
            if (route_matches(st->name, sig, &d)) {
                fprintf(stderr, "ROUTE: confirmed %s at vblank %u (step %d, d=%d)\n",
                        st->name, v, s_route_pc, d);
                route_advance();
                continue;
            }
            {
                int bd = 255, bi = route_best_match(sig, &bd);
                int wi = route_find(st->name);
                route_fail("line %d: EXPECT %s at vblank %u, but the screen is %s d=%d "
                           "(%s d=%d, match needs d<=%d)",
                           st->line, st->name, v,
                           bi >= 0 ? s_route_cp[bi].name : "<unknown>", bd, st->name, d,
                           wi >= 0 ? s_route_cp[wi].tol : s_route_tol);
            }
            return keys;
        }
        case ROUTE_OP_NID: {
            const char *nm = sr_nid_name(st->b);
            if (route_nid_since(st->b)) {
                fprintf(stderr, "ROUTE: guest called %s (0x%08x) at vblank %u (step %d, after "
                                "%u vblanks)\n",
                        nm ? nm : "an unnamed import", st->b, v, s_route_pc, el);
                route_advance();
                continue;
            }
            if (el >= st->a) {
                route_fail("line %d: WAIT_NID %s (0x%08x) was not called within %u vblanks "
                           "(from vblank %u to %u); the guest made %lu imports in that time, "
                           "none of them this one",
                           st->line, nm ? nm : "?", st->b, el, s_route_step_start, v,
                           s_route_nid_pos - s_route_nid_start);
                return keys;
            }
            return keys;
        }
        case ROUTE_OP_END:
            s_route_state = ROUTE_DONE;
            fprintf(stderr, "ROUTE_OK: %d steps completed by vblank %u\n", s_route_pc + 1, v);
            return keys;
        default:
            route_fail("line %d: corrupt route step", st->line);
            return keys;
        }
    }
    return keys;
}

int sr_route_status(void) { return s_route_state; }
int sr_route_sig_bytes(void) { return route_sig_bytes(); }

/* sceCtrl: sticks centred. To drive past the skippable intro movie and confirmation prompts
 * without a human, pulse START/CROSS/CIRCLE for a few frames on a periodic cadence (edge presses,
 * so the game sees press+release). Disable with SR_NOINPUT for a truly neutral pad. */
/* Sticky live-input latch for the auto-START pulse gate below. File-scope (rather than
 * function-static) so the selftest can reset and query it; production semantics are the
 * same either way. */
static int s_live_input_seen = 0;
/* The user asked for synthetic pad input by name: SR_PAD (the mask) or one of the pulse's
 * cadence knobs. An empty value is not a request. */
static int ctrl_pulse_requested(void) {
    static const char *const knobs[] = { "SR_PAD", "SR_PADPERIOD", "SR_PADWIDTH", "SR_PADSTART" };
    for (size_t i = 0; i < sizeof knobs / sizeof knobs[0]; i++) {
        const char *v = getenv(knobs[i]);
        if (v && v[0]) return 1;
    }
    return 0;
}
/* One-directional evidence gate: any nonzero live key mask proves a human is driving,
 * while a quiet frame proves nothing. Returns nonzero when the synthetic pulse must be
 * suppressed. Reads SR_NOINPUT and gui_pad_present() on every call so hotplug and
 * keyboard-first/gamepad-later sequences need no extra state.
 *
 * A person at a real window (gui_interactive(): not the offscreen sink, a headless host or an
 * acceptance launch) never gets a pulse they did not ask for, even before their first key:
 * the pulse is fake input there, and a demo that quits on START closes on its first frame. */
static int ctrl_pulse_suppressed(uint32_t keys) {
    if (keys) s_live_input_seen = 1;
    if (getenv("SR_NOINPUT") || gui_pad_present() || s_live_input_seen) return 1;
    if (gui_interactive() && !ctrl_pulse_requested()) return 1;
    return 0;
}
#ifdef SR_HLE_THREAD_SELFTEST
void sr_ctrl_test_reset_live_input(void) { s_live_input_seen = 0; }
int sr_ctrl_test_live_input_seen(void) { return s_live_input_seen; }
/* Exercise the exact production gate with synthetic key masks (the harness stubs
 * gui_on()/gui_buttons() to neutral, so production keys are always 0 there). */
int sr_ctrl_test_pulse_suppressed(uint32_t keys) { return ctrl_pulse_suppressed(keys); }
#endif
/* The headless auto-START pulse in guest time, like every scripted input: `mask` is pressed
 * from each nominal period boundary (none before `start`) for `width` samples and until the
 * guest has read it, then released until the guest has read the release and the next
 * boundary is due. On a host that keeps up this is the old `vcount % period < width` cadence;
 * on a starved one the guest still sees every pulse as a press and a release. A width that
 * covers the whole period is a constant hold. Called once per latched sample. */
static uint32_t auto_pulse_keys(uint32_t mask, uint32_t period, uint32_t width, uint32_t start,
                                uint32_t v) {
    if (width >= period) { sr_input_idle(&s_input); return v >= start ? mask : 0u; }
    if (!s_pulse.armed || s_pulse.mask != mask || s_pulse.period != period ||
        s_pulse.width != width || s_pulse.start != start) {
        /* The first pulse is the window the old cadence would be in now (or next): the
         * boundary at or before max(start, v) if that window is still open, else the next. */
        uint32_t base = v > start ? v : start;
        uint32_t boundary = base - base % period;
        s_pulse.armed = 1;
        s_pulse.mask = mask; s_pulse.period = period; s_pulse.width = width; s_pulse.start = start;
        s_pulse.next = base - boundary < width ? boundary : boundary + period;
        sr_input_idle(&s_input);
    }
    if (s_input.current) {
        if (!sr_input_delivered(&s_input)) return sr_input_mask(&s_input);
        if (s_input.seg.mask) {               /* the press was read: release it */
            SrInputSegment gap = { 0u, 1u, v, 0u, 0u };
            sr_input_start(&s_input, &gap, v);
            return 0u;
        }
    }
    if (v < s_pulse.next || v < start) return 0u;
    SrInputSegment press = { mask, width, v, 0u, 0u };
    sr_input_start(&s_input, &press, v);
    s_pulse.next = (v / period + 1u) * period;  /* the boundary after this press */
    return mask;
}

static uint32_t h_CtrlButtons(void) {
    /* Live keyboard (windowed mode) is OR'd with the auto-input pulse below -- in this headless
     * window environment no key is ever pressed, so without the pulse the intro movie never gets
     * its START and loops forever. SR_NOINPUT disables the pulse for a neutral pad. */
    uint32_t keys = gui_on() ? gui_buttons() : 0;
    /* SR_PADSCRIPT=<file>: either a route program (issue #64: state-qualified steps) or the
     * original absolute table of "frame hexmask width" lines -- press mask at frame for width
     * frames. Either way it replaces the default START pulse entirely, and either way its pad
     * state for this vblank was computed by route_tick() in guest time (scripted_input.h)
     * before the sample was latched. */
    route_load_once();
    if (s_route_state != ROUTE_OFF) return keys | s_route_keys;
    /* The auto-START pulse below only exists to advance the intro/attract in headless or no-input
     * runs. When a real controller is connected the player drives input themselves, so suppress the
     * pulse (otherwise a phantom START every few seconds would keep opening the pause menu).
     *
     * The same holds from the first vblank in a real window (gui_interactive()): the pulse
     * starts at vblank 0 and fires for 4 vblanks every 240, so a player who has not yet touched a
     * key would still be handed a phantom START -- the "Test this PC" demos quit on START and
     * closed on their first frame (walkthrough B16, reproduced with a plain mouse click). There
     * the pulse runs only when the user named it (SR_PAD, SR_PADPERIOD, SR_PADWIDTH or
     * SR_PADSTART). Headless runs (the offscreen sink, a dummy video driver, an acceptance
     * launch) and runs with an explicit SR_PAD or a route/pad script keep today's behaviour.
     *
     * A connected pad is not the only way a human drives this runtime: the keyboard path is
     * equally live, and gui_pad_present() is false for it (it reports gamepad presence only;
     * the GDI fallback sets it to 0 unconditionally while read_keys() still delivers live
     * keyboard state, and the SDL3 path likewise ORs keyboard state into the buttons while
     * s_pad_present tracks the gamepad alone). A keyboard player therefore used to get a
     * synthesised START for `width` vblanks every `period` -- 4.00 s on the defaults --
     * which opens the pause menu over and over and makes the session unplayable. So latch
     * the first live input from ANY source and stop synthesising from then on. The latch
     * is sticky because the evidence is one-directional: a press proves a human is present,
     * while a quiet frame proves nothing (players pause, read menus, and watch cutscenes
     * without touching anything, and that is exactly when a phantom START does the most
     * damage). Headless runs never set it (keys stays 0 there), so bootstrap pulsing is
     * preserved exactly where it is needed.
     *
     * `keys` cannot contain the pulse itself here -- the pulse is OR'd into the return
     * value below this point and never feeds back in -- so the latch cannot be
     * self-triggering. Route-script keys likewise never reach it: the route branch returns
     * above, so synthetic route playback neither sets the latch nor is gated by it. */
    if (ctrl_pulse_suppressed(keys)) { sr_input_idle(&s_input); return keys; }
    /* Pulse START only, briefly, on a slow cadence to skip the (minutes-long) intro movie and the
     * "press start" prompt. Pressing CROSS/CIRCLE as well drove the menus into bad states (it
     * confirmed things the game was not ready for); START alone advances the intro without that.
     * SR_PAD=<hex> overrides the mask; SR_PADPERIOD/SR_PADWIDTH tune the cadence. */
    const char *m = getenv("SR_PAD");
    uint32_t mask = m ? (uint32_t)strtoul(m, NULL, 16) : 0x0008u;   /* START */
    const char *pe = getenv("SR_PADPERIOD"); int period = pe ? atoi(pe) : 240;
    const char *pw = getenv("SR_PADWIDTH");  int width  = pw ? atoi(pw) : 4;
    const char *ps = getenv("SR_PADSTART");  int startf = ps ? atoi(ps) : 0;  /* hold input until frame */
    if (period <= 0) period = 240;
    if (width <= 0) return keys;
    return keys | auto_pulse_keys(mask, (uint32_t)period, (uint32_t)width,
                                  startf > 0 ? (uint32_t)startf : 0u, s_vcount_fwd);
}

void sr_ctrl_sample(void) {
    /* The pause request is already a scheduler rendezvous. Do not commit a pad
     * sample at the request boundary; the first post-resume sample owns that
     * guest-time input transition. */
    if (sched_pause_pending()) return;
    uint8_t lx = 128u, ly = 128u;
    if (s_ctrl_sampling_mode == 1u) {
        if (gui_on()) gui_analog(&lx, &ly);
#ifdef SR_HLE_THREAD_SELFTEST
        if (s_ctrl_test_stick) { lx = s_ctrl_test_lx; ly = s_ctrl_test_ly; }
#endif
    }
    uint32_t buttons = h_CtrlButtons();
    /* The on-screen keyboard is a system dialog: while it is open the title's pad reads
     * nothing, and the keyboard takes the pad instead. The scripted route reaches it as the
     * held mask; a live gamepad as its press events (gui.c). The auto-START pulse never does,
     * so a headless run cannot confirm a name it did not type. The held state is tracked
     * whether or not the keyboard is open, so a button already down does not act on it. */
    uint32_t pulses = gui_pad_pulses_take();
    int keyboard_took_sample = sr_osk_overlay_active();   /* open before this sample's press */
    sr_osk_overlay_pad(s_route_state != ROUTE_OFF ? s_route_keys : 0u, pulses);
    /* The keyboard owns every sample taken while it is open, including the one whose press
     * confirms or cancels it: that press closed the dialog and must not reach the title as a
     * START or CIRCLE in the same frame. Masking the pad while the keyboard is open is a
     * project choice, not yet measured on hardware. */
    keyboard_took_sample = keyboard_took_sample || sr_osk_overlay_active();
    if (keyboard_took_sample) buttons = 0u;
    if (hle_inlog_on()) {
        static uint32_t previous;
        if (buttons != previous) {
            fprintf(stderr, "ctrl_latch: vcount=%u buttons 0x%04x -> 0x%04x lx=%u ly=%u\n",
                    s_vcount_fwd, previous, buttons, lx, ly);
            previous = buttons;
        }
    }
    uint32_t input_id = sr_input_latch(&s_input);
    /* The on-screen keyboard reads every sample while it is open, as the system keyboard
     * polls the pad on a PSP, so a scripted press it took counts as read and the route moves
     * on; without this the route would wait for a guest read that the keyboard now holds back. */
    if (keyboard_took_sample) (void)sr_input_read(&s_input, input_id);
    /* With a sampling cycle the samples due before this VBLANK keep the previous input, and
     * the VBLANK itself latches none. */
    if (s_ctrl_sampling_cycle) ctrl_cycle_catch_up();
    s_ctrl_live = (CtrlSample){ buttons, 0u, lx, ly, input_id };
    if (gui_on()) gui_consume_button_pulses();
    if (s_ctrl_sampling_cycle) return;
    /* Stamped with the low 32 bits of the guest microsecond clock at latch. */
    ctrl_push(&s_ctrl_live, (uint32_t)sched_vtime_us());
    sched_wake(CTRL_WAIT_OBJ);
}
uint32_t s_framebuf = 0x04000000u, s_vcount = 0;

/* Number of no-frame watchdog observations emitted (see sr_vblank_tick). The
 * threshold alone is a NO-NEW-FLIP observation, not a hang verdict, so this
 * counter only says how often the threshold was crossed. File-local: the
 * selftest reads it through the accessor below, nothing else consumes it. */
unsigned long s_watchdog_fires = 0;

/* Test-only: restore the display statics to the values a fresh process starts
 * with.  sr_hle_init() latches once per process, so a suite that runs many
 * cases in one executable has no other way to isolate display state -- and
 * without it a case that changes the latched pixel format silently rejects the
 * next case's immediate flip.  Production never calls this. */
void sr_display_test_reset(void) {
    DisplayFrameState boot = { 0x04000000u, 512, 3 };
    s_display_active = boot;
    s_display_latched = boot;
    s_display_latched_pending = 0;
    s_display_configured = 0;
    s_framebuf = 0x04000000u;
    s_picture_probe_vcount = 0;
    memset(&s_picture_watch, 0, sizeof s_picture_watch);
}
#define GE_ENQUEUE_TRACE_THREADS 64

static GeEnqueueTraceHle s_ge_trace_hle[GE_ENQUEUE_TRACE_THREADS];
static GeEnqueueTraceCallback s_ge_trace_callback[GE_ENQUEUE_TRACE_THREADS];
uint64_t s_ge_trace_seq;

uint32_t ge_enqueue_trace_call_pc(const CpuState *s) {
    return s->r[31] >= 8u ? s->r[31] - 8u : 0u;
}

int ge_enqueue_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SR_GE_ENQUEUE_TRACE") != NULL;
    return enabled;
}

GeEnqueueTraceHle *ge_enqueue_trace_hle_slot(uint32_t uid) {
    GeEnqueueTraceHle *free_slot = NULL;
    for (int i = 0; i < GE_ENQUEUE_TRACE_THREADS; i++) {
        if (s_ge_trace_hle[i].uid == uid) return &s_ge_trace_hle[i];
        if (!s_ge_trace_hle[i].uid && !free_slot) free_slot = &s_ge_trace_hle[i];
    }
    return free_slot ? free_slot : &s_ge_trace_hle[uid % GE_ENQUEUE_TRACE_THREADS];
}

GeEnqueueTraceCallback *ge_enqueue_trace_callback_slot(uint32_t uid) {
    GeEnqueueTraceCallback *free_slot = NULL;
    for (int i = 0; i < GE_ENQUEUE_TRACE_THREADS; i++) {
        if (s_ge_trace_callback[i].uid == uid) return &s_ge_trace_callback[i];
        if (!s_ge_trace_callback[i].uid && !free_slot) free_slot = &s_ge_trace_callback[i];
    }
    return free_slot ? free_slot : &s_ge_trace_callback[uid % GE_ENQUEUE_TRACE_THREADS];
}

/* Once per delivered vblank, before the controller sample is latched, so a checkpoint
 * reached on this vblank can release its press on this vblank. */
void route_tick(uint32_t v) {
    route_load_once();
    if (s_route_state == ROUTE_LEGACY && !s_route_learn) { s_route_keys = route_legacy_tick(v); return; }
    /* A finished or failed program presses nothing more. (The auto-START pulse shares the
     * player only when no route is loaded, so ROUTE_OFF must not idle it here.) */
    if (s_route_state == ROUTE_DONE || s_route_state == ROUTE_FAILED) sr_input_idle(&s_input);
    if (s_route_state != ROUTE_RUNNING && !s_route_learn) { s_route_keys = 0; return; }

    uint8_t sig[ROUTE_SIG_MAX];
    const uint8_t *observed = NULL;
    /* Which steps need to see the screen, stated as "everything except the ones that do
     * not". A step that needs an observation and never gets one cannot make progress and
     * only reveals itself at its timeout, so the default is deliberately to sample: adding
     * a new state-gated step must not be able to silently disable its own observations. */
    int pending = 0;
    if (s_route_state == ROUTE_RUNNING && s_route_pc < s_route_nsteps) {
        int op = s_route_prog[s_route_pc].op;
        /* WAIT_NID is the one gated step that watches the guest rather than the screen, so it
         * needs no signature: sampling the framebuffer for it would cost the observer's ~20%
         * of vblank rate (measured) to learn nothing. */
        pending = !(op == ROUTE_OP_PRESS || op == ROUTE_OP_DELAY || op == ROUTE_OP_END ||
                    op == ROUTE_OP_NID || op == ROUTE_OP_UNTIL_NID);
    }
    /* Elapsed-delivered-VCOUNT cadence (#109 reconstruction): delivered VCOUNT is
     * elapsed-period accounting and may jump over every exact residue of
     * SAMPLE_EVERY, so sampling only when v % SAMPLE_EVERY == 0 could starve a
     * pending route of its whole observation budget despite valid frame
     * delivery. Instead: the FIRST pending attempt samples immediately, and
     * every later attempt whose unsigned elapsed VCOUNT reached SAMPLE_EVERY
     * samples again. The due attempt is recorded BEFORE framebuffer readback so
     * failed coherence/readback attempts stay cadence-bounded instead of
     * retrying every vblank. */
    int due = !s_route_have_attempt ||
              (uint32_t)(v - s_route_last_attempt) >= (uint32_t)s_route_sample_every;
    if ((pending || s_route_learn) && due) {
        s_route_last_attempt = v;
        s_route_have_attempt = 1;
        if (route_sample(sig)) {
            observed = sig;
            if (s_route_learn) route_learn_emit(v, sig);
        }
    }
    s_route_keys = s_route_state == ROUTE_LEGACY ? route_legacy_tick(v) : sr_route_step(v, observed);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Selftest-only entry into the real route_tick path: lets the executable
 * regression drive production sampling/cadence/state-machine behavior without
 * a scheduler, a title, or a GPU. Production builds compile none of this. */
void sr_route_test_tick(uint32_t v) { route_tick(v); }
/* Feed the import ring exactly as sr_syscall() does, gate included, and resolve a route's
 * name-or-hex token through the production resolver. A step that waits on a guest event is
 * only trustworthy if the observation it reads is the one the dispatcher writes. */
void sr_route_test_import(uint32_t nid) { if (s_route_watch_nid) route_note_nid(nid); }
uint32_t sr_route_test_nid(const char *tok) {
    uint32_t nid = 0;
    return route_nid_from_token(tok, &nid) == 0 ? nid : 0u;
}
/* Read-only view of the cadence bookkeeping: whether an attempt was recorded
 * and which delivered VCOUNT it holds. Pins the record-BEFORE-readback order
 * (a failed readback must still consume its cadence slot). */
int sr_route_test_cadence_state(uint32_t *last_attempt) {
    if (last_attempt) *last_attempt = s_route_last_attempt;
    return s_route_have_attempt;
}
#endif

void vramdump_note_vblank(uint32_t vcount) {
    vramdump_init_once();
    if (!s_vramdump_count) return;
    for (unsigned i = 0; i < s_vramdump_count; i++) {
        unsigned bit = 1u << i;
        if (!(s_vramdump_done & bit) && s_vramdump_vblanks[i] < vcount) {
            s_vramdump_done |= bit;
            fprintf(stderr, "VRAMDUMP vblank=%u NOT_CAPTURED (no frame was presented at that vblank)\n",
                    s_vramdump_vblanks[i]);
        }
    }
}

/* Called once per delivered VBLANK... */
void sr_ctrl_sample(void);

/* Guest-visible VCOUNT advances by elapsed display periods at scheduler
 * source-latch boundaries, decoupled from VBLANK service -- it is not the count
 * of delivered/serviced VBLANK episodes.  Reads remain observational: this is
 * deliberately not described as a strictly free-running register.  The source
 * latch advances the counter by the number of periods it just coalesced.
 * deliver_vblank() keeps calling sr_vblank_tick() exactly once per serviced
 * event; that service path must not also increment VCOUNT or a coalesced burst
 * would be double-counted. */
void sr_display_advance_vcount(uint32_t elapsed_periods) {
    s_vcount += elapsed_periods;
    s_vcount_fwd = s_vcount;
}
uint32_t sr_display_vcount(void) { return s_vcount; }

/* SR_SAMPLE_SPEC=<file>: a per-vblank record of guest state, for timing comparisons such as
 * an authentic run against an enhancement build of the same scripted route
 * (docs/DEBUGGING.md, "Sampling guest state per vblank"). Each spec line is
 * `<label> <type> <address> [<offset>...]`: start at <address>, and for each <offset> read the
 * guest word there as a pointer and add <offset>. <type> is u8, s8, u16, s16, u32, s32 or f32.
 * At the end of every serviced vblank one CSV row `vblank,<value>...` is appended to
 * SR_SAMPLE_OUT (default samples.csv); a value whose pointer chain leaves readable guest
 * memory is left empty. A spec the reader cannot use stops the run before it starts. Unset,
 * nothing is read or written. */
enum { HLE_SAMPLE_MAX = 32, HLE_SAMPLE_MAX_OFFSETS = 6 };
typedef struct {
    char label[48];
    char type[4];
    uint32_t base;
    uint32_t offsets[HLE_SAMPLE_MAX_OFFSETS];
    int offset_count;
} HleStateSample;
static HleStateSample s_samples[HLE_SAMPLE_MAX];
static int s_sample_count = -1;
static FILE *s_sample_out;

static void hle_state_sample_refuse(const char *path, int line, const char *why) {
    fprintf(stderr, "SR_SAMPLE_SPEC: %s line %d: %s; the run stops here\n", path, line, why);
    sr_perf_shutdown();
    fflush(stderr);
    _Exit(2);
}

static void hle_state_sample_init(void) {
    s_sample_count = 0;
    const char *path = getenv("SR_SAMPLE_SPEC");
    if (!path || !path[0]) return;
    FILE *spec = fopen(path, "r");
    if (!spec) hle_state_sample_refuse(path, 0, "cannot be read");
    char text[512];
    int line = 0;
    while (fgets(text, sizeof(text), spec)) {
        line++;
        char *hash = strchr(text, '#');
        if (hash) *hash = '\0';
        char *tokens[3 + HLE_SAMPLE_MAX_OFFSETS + 1];
        int n = 0;
        for (char *t = strtok(text, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            if (n == (int)(sizeof(tokens) / sizeof(tokens[0]))) hle_state_sample_refuse(path, line, "too many offsets");
            tokens[n++] = t;
        }
        if (n == 0) continue;
        if (n < 3) hle_state_sample_refuse(path, line, "expected <label> <type> <address> [<offset>...]");
        if (s_sample_count == HLE_SAMPLE_MAX) hle_state_sample_refuse(path, line, "more than 32 samples");
        HleStateSample *sample = &s_samples[s_sample_count];
        if (strlen(tokens[0]) >= sizeof(sample->label)) hle_state_sample_refuse(path, line, "label too long");
        for (const char *c = tokens[0]; *c; c++)
            if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                  *c == '_' || *c == '.'))
                hle_state_sample_refuse(path, line, "label must be [A-Za-z0-9_.]");
        static const char *const types[] = {"u8", "s8", "u16", "s16", "u32", "s32", "f32"};
        int known = 0;
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) known |= strcmp(tokens[1], types[i]) == 0;
        if (!known) hle_state_sample_refuse(path, line, "unknown type");
        uint32_t values[1 + HLE_SAMPLE_MAX_OFFSETS];
        for (int i = 2; i < n; i++) {
            char *end = NULL;
            unsigned long long v = strtoull(tokens[i], &end, 0);
            if (!end || *end || v > UINT32_MAX) hle_state_sample_refuse(path, line, "bad address or offset");
            values[i - 2] = (uint32_t)v;
        }
        snprintf(sample->label, sizeof(sample->label), "%s", tokens[0]);
        snprintf(sample->type, sizeof(sample->type), "%s", tokens[1]);
        sample->base = values[0];
        sample->offset_count = n - 3;
        for (int i = 0; i < sample->offset_count; i++) sample->offsets[i] = values[1 + i];
        s_sample_count++;
    }
    fclose(spec);
    if (s_sample_count == 0) hle_state_sample_refuse(path, line, "names no samples");
    const char *out = getenv("SR_SAMPLE_OUT");
    if (!out || !out[0]) out = "samples.csv";
    s_sample_out = fopen(out, "w");
    if (!s_sample_out) hle_state_sample_refuse(out, 0, "SR_SAMPLE_OUT cannot be written");
    fputs("vblank", s_sample_out);
    for (int i = 0; i < s_sample_count; i++) fprintf(s_sample_out, ",%s", s_samples[i].label);
    fputc('\n', s_sample_out);
}

void hle_state_sample_vblank(uint32_t vcount) {
    if (s_sample_count < 0) hle_state_sample_init();
    if (!s_sample_out) return;
    fprintf(s_sample_out, "%u", vcount);
    for (int i = 0; i < s_sample_count; i++) {
        const HleStateSample *sample = &s_samples[i];
        uint32_t addr = sample->base;
        int ok = 1;
        for (int k = 0; ok && k < sample->offset_count; k++) {
            ok = sr_guest_span_readable(addr, 4u);
            if (ok) addr = MEM_R32(addr) + sample->offsets[k];
        }
        uint32_t width = sample->type[1] == '8' ? 1u : sample->type[1] == '1' ? 2u : 4u;
        if (!ok || !sr_guest_span_readable(addr, width)) { fputc(',', s_sample_out); continue; }
        if (strcmp(sample->type, "u8") == 0) fprintf(s_sample_out, ",%u", (unsigned)MEM_R8(addr));
        else if (strcmp(sample->type, "s8") == 0) fprintf(s_sample_out, ",%d", (int)(int8_t)MEM_R8(addr));
        else if (strcmp(sample->type, "u16") == 0) fprintf(s_sample_out, ",%u", (unsigned)MEM_R16(addr));
        else if (strcmp(sample->type, "s16") == 0) fprintf(s_sample_out, ",%d", (int)(int16_t)MEM_R16(addr));
        else if (strcmp(sample->type, "u32") == 0) fprintf(s_sample_out, ",%u", (unsigned)MEM_R32(addr));
        else if (strcmp(sample->type, "s32") == 0) fprintf(s_sample_out, ",%d", (int)(int32_t)MEM_R32(addr));
        else {
            uint32_t bits = MEM_R32(addr);
            float f;
            memcpy(&f, &bits, sizeof(f));
            fprintf(s_sample_out, ",%.9g", (double)f);
        }
    }
    fputc('\n', s_sample_out);
}

void hle_state_sample_close(void) {
    if (s_sample_out) {
        fclose(s_sample_out);
        s_sample_out = NULL;
    }
}

/* 60 FPS logic speed (docs/ENHANCEMENT_CONTRACT.md section 9.5). An enhancement build with
 * the toggle on registers its logic-update counter. Once a second the runtime compares it with
 * the previous second: logic that runs but stays under 57 updates for three seconds in a row
 * plays slower than the original game (its per-update steps were halved), so it is reported
 * on stderr and through sr_enhancement_logic_slowed() for the presenter to show. A second with
 * no update at all (a load, a pause) is not a slowdown and clears the state. */
enum { HLE_ENH_SLOW_BELOW = 57, HLE_ENH_SLOW_SECONDS = 3 };
static const volatile uint32_t *s_enh_updates;
static uint32_t s_enh_updates_last;
static uint32_t s_enh_vblank_mark;
static uint32_t s_enh_slow_seconds;
static atomic_uint s_enh_slow_rate;   /* updates in the last second while slowed, else 0 */

void sr_enhancement_register_updates(const volatile uint32_t *counter) {
    s_enh_updates = counter;
    s_enh_updates_last = counter ? *counter : 0u;
    s_enh_vblank_mark = s_vcount;
    s_enh_slow_seconds = 0u;
    atomic_store(&s_enh_slow_rate, 0u);
}

int sr_enhancement_logic_slowed(unsigned *rate) {
    unsigned r = atomic_load(&s_enh_slow_rate);
    if (rate) *rate = r;
    return r != 0u;
}

void hle_enhancement_speed_vblank(uint32_t vcount) {
    if (!s_enh_updates || vcount - s_enh_vblank_mark < 60u) return;
    s_enh_vblank_mark = vcount;
    uint32_t now = *s_enh_updates;
    uint32_t delta = now - s_enh_updates_last;
    s_enh_updates_last = now;
    unsigned was = atomic_load(&s_enh_slow_rate);
    if (delta > 0u && delta < HLE_ENH_SLOW_BELOW) {
        if (++s_enh_slow_seconds >= HLE_ENH_SLOW_SECONDS) {
            if (!was)
                fprintf(stderr, "ENHANCEMENT_LOGIC_SLOWED: 60 FPS logic is running at %u updates/s; "
                                "the game plays slower than the original\n", (unsigned)delta);
            atomic_store(&s_enh_slow_rate, (unsigned)delta);
        }
        return;
    }
    if (was) fprintf(stderr, "ENHANCEMENT_LOGIC_SLOWED: cleared (%u updates/s)\n", (unsigned)delta);
    s_enh_slow_seconds = 0u;
    atomic_store(&s_enh_slow_rate, 0u);
}

/* Self-test seams: one sample row, and a reset that re-reads SR_SAMPLE_SPEC. */
void sr_hle_test_state_sample_vblank(uint32_t vcount) { hle_state_sample_vblank(vcount); }
void sr_hle_test_state_sample_reset(void) {
    hle_state_sample_close();
    s_sample_count = -1;
}
void sr_hle_test_enhancement_speed_vblank(uint32_t vcount) { hle_enhancement_speed_vblank(vcount); }
GeCallback s_ge_cb[16];

/* Every nested guest call this runtime makes -- GE callbacks here and the MPEG
 * ring-refill callback in mpeg.c -- runs on a guest stack that is NOT the
 * calling thread's own $sp.  It used to be one hard-coded address (0x09df8000)
 * shared by every such call, on every thread, at every depth; nested_frames.c
 * replaces that with a per-owner, per-depth frame out of a reserved region.
 *
 * Only $sp changes here.  The zeroed CpuState, the inherited $gp, $ra = 0, the
 * 0xe4 VFPU prefix seeds and the full caller-state restore are unchanged, and
 * whether they match the PSP remains NOT_ESTABLISHED -- see the callback-ABI
 * regression in hle_thread_selftest.c, which measures what this runtime does. */
static void ge_call_guest(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t frame_sp = 0;
    int frame = -1;
    if (!fn) { fprintf(stderr, "GE_CALL_GUEST: fn=0x0 (null, skipping)\n"); return; }
    /* Fail closed BEFORE any caller state is touched: with no frame of its own
     * there is no correct stack to hand the callee, and dispatching onto a frame
     * another owner holds is the defect this replaced. */
    if (!sr_nested_frame_acquire(sched_current_uid(), &frame_sp, &frame)) {
        fprintf(stderr, "GE_CALL_GUEST: fn=0x%08x refused -- no nested guest-call frame "
                        "available (cur_uid=0x%x)\n", fn, sched_current_uid());
        return;
    }
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST: fn=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x cur_uid=0x%x sp=0x%08x\n",
                fn, a0, a1, a2, sched_current_uid(), frame_sp);
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = save.r[28];
    s->r[29] = frame_sp;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    s->pc = fn;
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    uint32_t callback_fatal_target = 0u;
    int callback_fatal = hle_dispatch_guest_scoped(
        s, fn, "GE guest callback", &callback_fatal_target);
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST: fn=0x%08x returned, v0=0x%08x\n", fn, s->r[2]);
    memcpy(s, &save, sizeof(CpuState));
    if (callback_fatal) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = callback_fatal_target;
    }
    /* Returning to the interrupted context ends any ll..sc window it had open. */
    sr_cpu_link_clear(s);
    atomic_store_explicit(&sr_timeslice, save_slice, memory_order_relaxed);
    (void)sr_nested_frame_release(frame);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Reach the production nested-guest-call marshalling from the executable
 * selftest.  ge_call_guest_rv() is static and its callers all need real GE or
 * MPEG state, so without this hook the only way to characterise the callback
 * ABI would be to re-implement it in the test -- which would measure the copy,
 * not the contract.  This adds no production behaviour: it is a call-through,
 * compiled only into the test executable. */
uint32_t sr_hle_test_call_guest(CpuState *s, uint32_t fn,
                                uint32_t a0, uint32_t a1, uint32_t a2) {
    return ge_call_guest_rv(s, fn, a0, a1, a2);
}
/* Reach the void-returning marshaller too: it carries its own acquire/release
 * pair, so "both wrappers isolate" is a claim the regression has to test rather
 * than infer from the one it can already call. */
void sr_hle_test_call_guest_void(CpuState *s, uint32_t fn,
                                 uint32_t a0, uint32_t a1, uint32_t a2) {
    ge_call_guest(s, fn, a0, a1, a2);
}
#endif /* SR_HLE_THREAD_SELFTEST */

/* A list with no usable finish callback is reported once per callback ID, not once per list:
 * a title that enqueues lists against an unregistered ID does so every frame, and one line per
 * list grew a log by about 85 MB an hour. Bit i is ID i; bit 16 is any ID past the table. */
static uint32_t s_ge_finish_reported;
static unsigned s_ge_finish_report_lines;

static int ge_finish_report_once(unsigned bit) {
    if (s_ge_finish_reported & (1u << bit)) return 0;
    s_ge_finish_reported |= 1u << bit;
    s_ge_finish_report_lines++;
    return 1;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only view of the finish-callback diagnostics: the lines printed so far. */
unsigned sr_hle_test_ge_finish_reports(int reset) {
    unsigned lines = s_ge_finish_report_lines;
    if (reset) s_ge_finish_reported = 0u, s_ge_finish_report_lines = 0u;
    return lines;
}
#endif

/* Run the guest's registered finish callback for a completed display list, or log why
 * none ran. The runtime advances no title counter of its own here.
 *
 * The callback is called as finish_func(id, arg) (PSPSDK pspge.h PspGeCallback), where id is
 * the 16-bit argument of the list's FINISH command, not the queue ID: libpspgu's callbackFin
 * passes id & 0xffff on as sceGuFinishId's id. The PSP hands a SIGNAL's argument to the signal
 * callback the same way (PSP-GE-TIMING-001 c4, measured on a PSP-3000); the FINISH id itself
 * is UNMEASURED. A title that marks one kind of frame with its own FINISH id waited forever
 * for a callback that, given the queue ID, never saw it. */
void ge_finish_callback(CpuState *s, uint32_t cbid, uint32_t finish_id, uint32_t user_arg) {
    const size_t cb_max = sizeof(s_ge_cb) / sizeof(s_ge_cb[0]);
    /* sceGeListEnQueue's callback ID is a signed int, and a negative one (the SDK's -1) is a
     * list enqueued with no callback, so there is nothing to run and nothing to report. */
    if ((int32_t)cbid < 0) return;
    if (cbid >= (uint32_t)cb_max) {
        if (ge_finish_report_once(16u))
            fprintf(stderr, "GE_FINISH_CB: cbid=%u OUT OF RANGE (max %zu) -- no guest cb "
                    "(reported once)\n", cbid, cb_max);
        return;
    }
    GeCallback *cb = &s_ge_cb[cbid];
    if (!cb->used || !cb->finish_func) {
        if (ge_finish_report_once((unsigned)cbid))
            fprintf(stderr, "GE_FINISH_CB: cbid=%u used=%d fn=0x%08x (SKIPPED - %s) "
                    "(reported once per cbid)\n",
                    cbid, cb->used, cb->finish_func, !cb->used ? "not registered" : "no function");
        return;
    }
    if (ge_log_on())
        fprintf(stderr, "GE_FINISH_CB: cbid=%u id=0x%04x fn=0x%08x arg=0x%08x\n",
                cbid, finish_id, cb->finish_func, cb->finish_arg ? cb->finish_arg : user_arg);
    ge_call_guest(s, cb->finish_func, finish_id, cb->finish_arg ? cb->finish_arg : user_arg, cbid);
    if (ge_log_on())
        fprintf(stderr, "GE_FINISH_CB: callback fn=0x%08x returned\n", cb->finish_func);
}

void ge_sync_wake(void) { sched_wake(GE_SYNC_WAIT_OBJ); }

GeListInfo s_ge_lists[GE_LIST_MAX];
uint64_t s_ge_list_done_bits = UINT64_MAX; /* completion bit per slot */
uint8_t s_ge_free_slots[GE_LIST_MAX];      /* FIFO of free slots */
unsigned s_ge_free_head, s_ge_free_count;

void ge_free_push(int slot) {
    s_ge_free_slots[(s_ge_free_head + s_ge_free_count) % GE_LIST_MAX] = (uint8_t)slot;
    s_ge_free_count++;
}

uint64_t ge_slot_bit(int slot) { return (uint64_t)1 << slot; }

/* A list completed: its slot joins the free FIFO, its waiters are released. */
void ge_list_completed(int slot) {
    s_ge_lists[slot].status = GE_LIST_COMPLETED;
    ge_free_push(slot);
    s_ge_list_done_bits |= ge_slot_bit(slot);
    ge_sync_wake();
}
