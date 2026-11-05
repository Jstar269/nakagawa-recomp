/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace.h - the Enhanced and Ace play modes' runtime side (docs/ENHANCEMENT_CONTRACT.md section
 * 10). The internal name stays "ace"; the launcher calls the modes Enhanced and Ace (#2055).
 *
 * One launch variable carries the run's choice. The launcher always exports it, so an inherited
 * value can never opt a launch in:
 *
 *     SR_ACE=off
 *     SR_ACE=v1;tier=enhanced;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off
 *
 * tier (enhanced or ace) only names the run, in the badge, the pause menu and the stamps; a
 * line without it is an Enhanced run. Every other key is required in v1, in any order, each
 * once. A key or value for a feature this build
 * does not carry (wide=fill, smooth=match) is still parsed and refused. Anything else (an unknown version, key
 * or value, a duplicate, a missing key, an empty field) refuses the whole line: Ace is off for
 * the run and one "ACE_MODE: refused (<reason>)" line says why. Unset is off and says nothing.
 *
 * Off is authentic: every Ace branch in the runtime sits behind g_ace_on, which stays 0, so the
 * Vulkan device, every sampler, the guest shaders, the presents and the observation counters
 * are today's, and nothing is printed. With Ace on, guest logic and the guest timing model are
 * unchanged; host presentation changes (and, with sas=sinc, the mixed SAS sample values), and
 * the run says so ("ACE_MODE: on (...)" at start, an exit line, and the flight recorder's
 * runtime.enhancements block).
 *
 * This unit uses the C library only, and on Windows MoveFileEx for the report's atomic replace.
 * ace-selftest links it with nothing else, so no code path here can reach a guest-visible
 * function (invariant S1). Callers pass the clock in.
 */
#ifndef SR_ACE_H
#define SR_ACE_H

#include <stddef.h>
#include <stdint.h>

#define ACE_SPEC_MAX 192         /* SR_ACE value, terminator included; room for later parts */

/* Features that are part of this build. A feature that is not must be off in SR_ACE. */
#define ACE_HAS_WIDE 0
#define ACE_HAS_SMOOTH 1        /* smooth=x2: one in-between per 30 Hz frame */
#define ACE_HAS_SMOOTH_MATCH 0  /* smooth=match: the monitor's rate */
#define ACE_HAS_SAS 1           /* sas=sinc: band-limited SAS voice resampling (#2054) */

typedef enum { ACE_WIDE_OFF = 0, ACE_WIDE_FILL = 1 } AceWide;
typedef enum { ACE_SMOOTH_OFF = 0, ACE_SMOOTH_MATCH = 1, ACE_SMOOTH_X2 = 2 } AceSmooth;
/* The play mode a run is: Enhanced, or Ace (Enhanced plus the game's own package). */
typedef enum { ACE_TIER_ENHANCED = 0, ACE_TIER_ACE = 1 } AceTier;
typedef enum { ACE_SAS_OFF = 0, ACE_SAS_SINC = 1 } AceSas;

typedef struct {
    int on;          /* 0: authentic (off, unset or refused); every field below is then 0 */
    int af;          /* texture filtering: 0, 2, 4, 8 or 16 */
    int text;        /* crisp system text: the on-screen keyboard at the window's resolution */
    int readout;     /* the speed readout chip */
    int notice;      /* the launch notice chip */
    int wide;        /* AceWide */
    int smooth;      /* AceSmooth */
    int tier;        /* AceTier */
    int sas;         /* AceSas: SAS voices mix through a band-limited resampler (host-only) */
} AceConfig;

typedef enum {
    ACE_FEATURE_AF = 0,
    ACE_FEATURE_TEXT,
    ACE_FEATURE_READOUT,
    ACE_FEATURE_NOTICE,
    ACE_FEATURE_WIDE,
    ACE_FEATURE_SMOOTH,
    ACE_FEATURE_SAS,
    ACE_FEATURE_COUNT
} AceFeature;

/* ---- the launch line ------------------------------------------------------------------------ */

/* Parses one SR_ACE value. Returns 1 for a valid line ("off" fills an all-zero config) and 0
 * for a refusal: then *out is all zero and `why` (when given) names the first problem. NULL is
 * refused as "unset"; ace_init treats unset as off without a refusal line. */
int ace_parse(const char *spec, AceConfig *out, char *why, size_t why_cap);
/* Writes the canonical line for a config ("off" when !c->on). Returns 0 when it does not fit. */
int ace_format(const AceConfig *c, char *out, size_t cap);

/* ---- the run's switch ----------------------------------------------------------------------- */

extern int g_ace_on;   /* the run's switch: 1 only after a valid line with Ace on */

/* Reads SR_ACE once (and SR_ACE_REPORT when on), prints the start stamp or the refusal, and
 * sets g_ace_on. Returns g_ace_on. ace_init_spec does the same for a given value (tests). */
int ace_init_from_env(void);
int ace_init_spec(const char *spec, const char *report_path);
/* The requested line, and what this host gave (a feature it cannot provide is off here). */
const AceConfig *ace_requested(void);
const AceConfig *ace_effective(void);
/* The canonical line of the run ("off" for an authentic run). */
const char *ace_spec_text(void);
/* What the run is called on screen: "Enhanced", or "Ace" for a tier=ace line. */
const char *ace_tier_name(void);
/* A feature this host cannot provide: off for the run, named once on stderr and in the report.
 * The others stay on. */
void ace_feature_unavailable(AceFeature feature, const char *reason);
/* A feature that really ran: texture filtering once the GPU renderer made its pool for it, the
 * in-game UI's three once its pass is registered. At exit a feature that is on but was never
 * confirmed is reported unavailable (its renderer or presenter did not start), so the report
 * never claims what did not happen. */
void ace_confirm(AceFeature feature);
/* An anisotropic sampler the renderer could not make: that draw kept the plain one. */
void ace_note_af_failure(void);
/* The pause menu's live Speed readout switch (Ace on only; a readout this host cannot draw
 * stays off). */
void ace_set_readout(int on);
/* The anisotropy the device gives, at most the requested level (0 = none). */
void ace_set_af_effective(int af, const char *reason);
/* The active features in words, joined by " \xC2\xB7 " ("16x filtering \xC2\xB7 crisp keyboard"),
 * for the pause menu caption and the launch notice. Empty with Ace off. */
size_t ace_feature_words(char *out, size_t cap);

/* ---- observation: the 30 Hz detector -------------------------------------------------------- */

/* The game's logic rate as VBLANKs per new frame. RATE_K1..K4 are 60, 30, 20 and 15 Hz. */
typedef enum {
    ACE_RATE_NONE = 0,        /* no class is stable */
    ACE_RATE_K1 = 1,
    ACE_RATE_K2 = 2,
    ACE_RATE_K3 = 3,
    ACE_RATE_K4 = 4,
    ACE_RATE_STATIC = 5,      /* no new frame for ACE_DET_STATIC_VBLANKS */
    ACE_RATE_VARIABLE = 6,    /* frames come at no one rate */
    ACE_RATE_COUNT
} AceRate;

#define ACE_DET_WINDOW 32u          /* intervals the class is judged over */
#define ACE_DET_STATIC_VBLANKS 30u  /* no new frame for this long reads STATIC */
#define ACE_DET_ENTER_VBLANKS 60u   /* a class is entered after 1.0 s of agreement */
#define ACE_DET_LEAVE_VBLANKS 30u   /* and left after 0.5 s of disagreement */

typedef struct {
    uint8_t interval[ACE_DET_WINDOW]; /* VBLANKs between consecutive new frames, ring */
    uint32_t count;                   /* intervals held, at most ACE_DET_WINDOW */
    uint32_t head;                    /* next slot to write */
    uint32_t last_new_vcount;
    int have_last;
    AceRate candidate;                /* the window's class, while it differs from stable */
    uint32_t candidate_since;
    AceRate stable;
    int disagreeing;
    uint32_t disagree_since;
    AceRate last_rate;                /* the last stable K class, kept through STATIC */
    int ever_stable;                  /* a K class has been stable once */
} AceDetector;

void ace_det_reset(AceDetector *d);
/* The class of a full window, or NONE before it is full. Class k (1 to 4) when the intervals
 * sum to between 32k - 2 and 32k + 4 and at least 28 of them are within one VBLANK of k; that
 * holds for "28 of 32 equal k, the rest k + 1" and for a 60 Hz game whose flips sometimes
 * share a slot (0, then 2). Otherwise VARIABLE. */
AceRate ace_det_window_class(const AceDetector *d);
/* One flip at `vcount`; new_frame is nonzero when the flipped target changed since its own
 * previous flip. Returns 1 when the stable class changed. */
int ace_det_flip(AceDetector *d, uint32_t vcount, int new_frame);
const char *ace_rate_name(AceRate rate);   /* "60 Hz", "30 Hz", ..., "static", "variable" */

/* ---- observation: the speed readout --------------------------------------------------------- */

/* One one-second window of observations. */
typedef struct {
    uint64_t wall_ns;          /* the window's length */
    uint32_t new_frames;       /* F's numerator */
    uint64_t idle_ns;          /* host time in the scheduler's two waits */
    uint32_t skipped_slots;    /* VBLANK deadlines vblank_pace had to skip */
    AceRate stable;            /* the detector's class at the window's end */
    int ever_stable;
    AceRate last_rate;         /* the last stable K class */
} AceWindow;

typedef struct {
    int in_stretch;            /* inside a PC-limited stretch */
    int stretch_k;             /* the K class that was stable when it began (0: none yet) */
} AceReadoutState;

typedef struct {
    double fps;                /* F: new frames per second */
    double idle_share;         /* I: host idle time over wall time */
    int pc_limited;
    int have_target;           /* a percentage can be shown */
    double target_fps;         /* T = 59.94 / k */
    int speed_pct;             /* F / T while PC limited, else 100 */
    char text[64];             /* "100%  30 FPS", "\xE2\x96\xB2 87%  26 FPS  PC LIMITED", "\xE2\x80\x94 27 FPS" */
} AceReadoutView;

void ace_readout_reset(AceReadoutState *s);
/* Judges one window; PC limited when a slot was skipped, or when the host idled under 2% while
 * frames came slower than the class allows. The target keeps the class stable when the stretch
 * began, so a steady slowdown does not redefine it. */
void ace_readout_judge(AceReadoutState *s, const AceWindow *w, AceReadoutView *out);

/* ---- observation feed (Ace on only; callers check g_ace_on) ---------------------------------- */

/* A draw or block transfer was recorded for the target at VRAM offset `fb`. Counted when it is
 * recorded, before the target's batches are submitted, so a flip that latches them sees it. */
void ace_note_target_draw(uint32_t fb);
/* A CPU write (a store, a movie frame, a software-raster result) reached VRAM offsets
 * [off, off + bytes): every flipped buffer it overlaps has changed. */
void ace_note_vram_write(uint32_t off, uint32_t bytes);
/* Host time the scheduler spent waiting (vblank_pace, sleep_until_us), and slots it skipped. */
void ace_note_host_wait(uint64_t ns);
void ace_note_skipped_slots(uint32_t slots);
/* The display flipped to the `bytes`-long buffer at VRAM offset `fb` at `vcount`
 * (display_present_active, before the presenter's own slot rule). Feeds the detector and,
 * once a second of `now_ns`, the readout. Returns 1 when the flip shows a new frame: the
 * buffer was drawn or written since its own previous flip, or it is not the buffer the
 * previous flip named. */
int ace_flip(uint32_t fb, uint32_t bytes, uint32_t vcount, uint64_t now_ns);
/* The readout as last judged; 0 before the first window. */
int ace_readout_view(AceReadoutView *out);
AceRate ace_rate_stable(void);
/* Observation counters (presenter cell A3: all zero with Ace off). */
typedef struct {
    uint64_t draws, flips, new_frames, host_wait_ns, skipped_slots, windows;
} AceCounters;
void ace_counters(AceCounters *out);

/* ---- smooth motion: which frame each present shows ------------------------------------------- */

/* A title that flips a new frame every second VBLANK leaves every other VBLANK without a present.
 * Smooth motion fills it: the flip of frame B shows the in-between of A and B, and the VBLANK
 * after it shows B, so B reaches the screen one VBLANK late and nothing is ever guessed ahead of
 * the guest. Host side only: these presents read private copies (gpu_sdl3vk/ace_smooth.h), and
 * the flip's own present still gives the capture and guest VRAM the real frame.
 *
 * Only the detector's stable 30 Hz class interpolates. Anything else shows each frame as it
 * comes (and keeps a copy, so the first frame of a 30 Hz stretch has a predecessor). */
typedef enum {
    ACE_SHOW_REAL = 0,     /* no new frame, or smooth off: this present is today's */
    ACE_SHOW_KEEP,         /* a new frame outside 30 Hz: show it and keep a copy */
    ACE_SHOW_BETWEEN       /* a new 30 Hz frame: show the in-between, hold the frame */
} AceShow;

typedef struct {
    AceShow show;          /* the flip's verdict, until its present takes it */
    int held;              /* a frame waits for its VBLANK */
    uint32_t held_after;   /* the VCOUNT of the present that holds it */
    uint64_t flip_ns;      /* when the latest flip arrived */
    uint64_t held_ns;      /* when the in-between of the held frame was presented */
    int reflip;            /* this flip shows the held frame again: it is the held present */
} AcePace;

void ace_pace_reset(AcePace *p);
/* A flip. The verdict stands until ace_pace_take. A new frame supersedes a frame still held; a
 * flip of the unchanged frame (a title that flips each frame twice) shows it, so that present
 * is the held one and the VBLANK has nothing left to show. */
void ace_pace_flip(AcePace *p, int new_frame, AceRate rate, int smooth, uint64_t now_ns);
/* The flip's present: returns the verdict and clears it. */
AceShow ace_pace_take(AcePace *p);
/* The in-between went to the screen at now_ns (the presenter's clock): the frame is held for the
 * VBLANK after `vcount`. Without smooth motion the frame would have been shown then, so the
 * held present's time less this one is the delay smooth motion adds. */
void ace_pace_shown_between(AcePace *p, uint32_t vcount, uint64_t now_ns);
/* One VBLANK. Returns 1 when the held frame is due; it is then no longer held. */
int ace_pace_vblank(AcePace *p, uint32_t vcount);

typedef enum {
    ACE_PRESENT_REAL = 0,  /* the flip's own frame */
    ACE_PRESENT_BETWEEN,   /* an in-between */
    ACE_PRESENT_HELD,      /* the held frame, one VBLANK after its flip */
    ACE_PRESENT_KINDS
} AcePresentKind;

/* Present-to-present time in 1 ms bins, the last bin open-ended. */
#define ACE_PACE_BINS 50u

typedef struct {
    uint64_t kind[ACE_PRESENT_KINDS];
    uint64_t interval[ACE_PACE_BINS];
    uint64_t interval_count;
    uint64_t interval_sum_ns;
    uint64_t hold_count;
    uint64_t hold_sum_ns;         /* flip to held present: the delay smooth motion adds */
    uint64_t hold_max_ns;
    uint64_t superseded;          /* a held frame a later flip replaced */
    uint64_t last_ns;
} AcePaceStats;

/* The kind of the flip's present when it shows no in-between: REAL, or HELD for a re-flip. */
AcePresentKind ace_pace_flip_kind(const AcePace *p);

/* The run's verdict for the flip ace_flip just saw. */
AceShow ace_smooth_take(void);
void ace_smooth_shown_between(uint64_t now_ns);
/* One VBLANK: 1 when the held frame should be presented now. Off, it reads nothing. */
int ace_smooth_vblank(uint32_t vcount);
/* A present reached the swapchain. A flip's present that showed no in-between is passed as
 * REAL and counted as HELD when it re-showed the held frame. */
void ace_smooth_note_present(AcePresentKind kind, uint64_t now_ns);
void ace_pace_stats(AcePaceStats *out);
void ace_pace_stats_add(AcePaceStats *s, AcePresentKind kind, uint64_t now_ns);

/* ---- the per-run report -------------------------------------------------------------------- */

#define ACE_REPORT_MAX 4096u
/* The report's JSON for the run so far; 0 when it would not fit. */
size_t ace_report_json(char *out, size_t cap);
/* Settles the features that never ran (ace_confirm), then writes the report (SR_ACE_REPORT,
 * through a temporary file that atomically replaces the old report) and the exit stamp. An
 * empty SR_ACE_REPORT writes no file. Ace off: does nothing. Safe to call more than once. */
void ace_finish(void);

#endif /* SR_ACE_H */
