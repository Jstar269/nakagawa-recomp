/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_selftest.c - Ace Mode's CPU-only cells (ace.h): the launch line fails closed (C1), the
 * 30 Hz detector (C4), the speed readout (C5), the per-run report, and the off run's silence.
 *
 * Built from ace.c and this file alone, with nothing else of the runtime: a reference from
 * ace.c to any runtime function fails the link, which is invariant S1's link test for ace.c.
 */
#include "ace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

static const char *const k_good = "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off";

/* ---- C1: the launch line ------------------------------------------------------------------- */

static void cell_parse(void) {
    AceConfig c;
    char why[64];
    CHECK(ace_parse(k_good, &c, why, sizeof(why)) == 1, "the canonical line parses (%s)", why);
    CHECK(c.on == 1 && c.af == 16 && c.text == 1 && c.readout == 0 && c.notice == 1 &&
          c.wide == 0 && c.smooth == 0, "fields of the canonical line");
    CHECK(ace_parse("v1;smooth=off;wide=off;notice=off;readout=on;text=off;af=0", &c, why,
                    sizeof(why)) == 1 && c.on && c.af == 0 && c.readout == 1,
          "keys in any order");
    CHECK(ace_parse("off", &c, why, sizeof(why)) == 1 && c.on == 0, "off is valid and off");

    static const struct { const char *spec; const char *why; } refused[] = {
        { NULL, "unset" },
        { "", "empty field" },
        { "on", "unknown version" },
        { "v2;af=16;text=on;readout=off;notice=on;wide=off;smooth=off", "unknown version" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;boost=on", "unknown key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;af=8", "duplicate key" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off", "missing key" },
        { "v1;af=3;text=on;readout=off;notice=on;wide=off;smooth=off", "bad value" },
        { "v1;af=16;text=yes;readout=off;notice=on;wide=off;smooth=off", "bad value" },
        { "v1;af=16;text=On;readout=off;notice=on;wide=off;smooth=off", "bad value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;", "empty field" },
        { "v1;;af=16;text=on;readout=off;notice=on;wide=off;smooth=off", "empty field" },
        { "v1;af;text=on;readout=off;notice=on;wide=off;smooth=off", "field is not key=value" },
        { "v1;af=;text=on;readout=off;notice=on;wide=off;smooth=off", "field is not key=value" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=fill;smooth=off", "wide is not part of this build" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=match", "smooth is not part of this build" },
        { "v1;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
          "line too long" },
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        memset(&c, 0x5A, sizeof(c));
        int ok = ace_parse(refused[i].spec, &c, why, sizeof(why));
        CHECK(ok == 0, "refused[%zu] parsed", i);
        CHECK(c.on == 0 && c.af == 0 && c.text == 0 && c.notice == 0, "refused[%zu] left fields set", i);
        CHECK(strcmp(why, refused[i].why) == 0, "refused[%zu]: reason '%s', expected '%s'", i, why,
              refused[i].why);
    }

    /* format and parse agree */
    char line[ACE_SPEC_MAX];
    AceConfig a = { 1, 4, 0, 1, 0, 0, 0 }, b;
    CHECK(ace_format(&a, line, sizeof(line)) && ace_parse(line, &b, NULL, 0) &&
          memcmp(&a, &b, sizeof(a)) == 0, "format round trip (%s)", line);
    CHECK(ace_format(&a, line, 8) == 0 && line[0] == '\0', "format refuses a short buffer");
    a.on = 0;
    CHECK(ace_format(&a, line, sizeof(line)) && strcmp(line, "off") == 0, "off formats as off");
}

/* ---- the run's switch and its silence ------------------------------------------------------- */

static void cell_switch(void) {
    AceCounters n;
    CHECK(ace_init_spec(NULL, NULL) == 0 && g_ace_on == 0, "unset is off");
    CHECK(ace_init_spec("off", NULL) == 0 && g_ace_on == 0, "off is off");
    CHECK(strcmp(ace_spec_text(), "off") == 0, "off run's line");
    /* A3's counter part: with Ace off the feed does nothing. */
    ace_note_target_draw(0x44000u);
    ace_note_host_wait(1000000u);
    ace_note_skipped_slots(3u);
    CHECK(ace_flip(0x44000u, 0x22000u, 1u, 1000u) == 0, "an off flip reports nothing");
    ace_counters(&n);
    CHECK(!n.draws && !n.flips && !n.new_frames && !n.host_wait_ns && !n.skipped_slots && !n.windows,
          "off counters stay zero");
    char words[128];
    CHECK(ace_feature_words(words, sizeof(words)) == 0 && words[0] == '\0', "no words with Ace off");
    ace_feature_unavailable(ACE_FEATURE_AF, "test");
    CHECK(ace_effective()->af == 0 && ace_requested()->on == 0, "off ignores capability calls");

    CHECK(ace_init_spec("v1;af=16;text=on;readout=on;notice=on;wide=off;smooth=off;bad", NULL) == 0 &&
          g_ace_on == 0, "a refused line is off");
    CHECK(ace_init_spec(k_good, NULL) == 1 && g_ace_on == 1, "the canonical line is on");
    CHECK(strcmp(ace_spec_text(), k_good) == 0, "the run's line is canonical");
    CHECK(ace_feature_words(words, sizeof(words)) > 0 &&
          strcmp(words, "16x filtering \xC2\xB7 crisp keyboard") == 0, "feature words: '%s'", words);
    ace_set_af_effective(8, NULL);
    CHECK(ace_effective()->af == 8 && ace_requested()->af == 16, "the device clamps filtering");
    ace_feature_unavailable(ACE_FEATURE_TEXT, "no window");
    ace_feature_unavailable(ACE_FEATURE_TEXT, "named once");
    CHECK(ace_effective()->text == 0 && ace_effective()->notice == 1, "one feature off, the rest on");
    ace_set_af_effective(0, "no samplerAnisotropy");
    CHECK(ace_effective()->af == 0, "no anisotropy turns filtering off");
}

/* ---- C4: the 30 Hz detector ----------------------------------------------------------------- */

/* Feeds new frames at the given intervals (cycled) for `vblanks` VBLANKs; returns the class. */
static AceRate det_run(AceDetector *d, uint32_t *vc, const uint8_t *gaps, size_t ngaps,
                       uint32_t vblanks) {
    uint32_t end = *vc + vblanks;
    size_t i = 0;
    while (*vc < end) {
        *vc += gaps[i % ngaps];
        (void)ace_det_flip(d, *vc, 1);
        i++;
    }
    return d->stable;
}

static void cell_detector(void) {
    AceDetector d;
    uint32_t vc = 1000;
    static const uint8_t k1[] = {1}, k2[] = {2}, k3[] = {3}, k4[] = {4};
    struct { const uint8_t *g; size_t n; AceRate want; } rates[] = {
        { k1, 1, ACE_RATE_K1 }, { k2, 1, ACE_RATE_K2 }, { k3, 1, ACE_RATE_K3 }, { k4, 1, ACE_RATE_K4 },
    };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, rates[r].g, rates[r].n, 400) == rates[r].want,
              "steady %s reads %s", ace_rate_name(rates[r].want), ace_rate_name(d.stable));
    }

    /* Entry timing: no class before the window is full and 1.0 s has agreed. */
    ace_det_reset(&d);
    vc = 1000;
    uint32_t first_stable = 0;
    for (int i = 0; i < 200 && !first_stable; i++) {
        vc += 2;
        (void)ace_det_flip(&d, vc, 1);
        if (d.stable == ACE_RATE_K2) first_stable = vc;
    }
    /* The window fills after 33 frames (vcount 1066); the class then needs 60 VBLANKs. */
    CHECK(first_stable >= 1066u + ACE_DET_ENTER_VBLANKS && first_stable <= 1066u + ACE_DET_ENTER_VBLANKS + 2u,
          "30 Hz entered at vcount %u", first_stable);

    /* A dropped frame now and then (28 of 32 equal 2, the rest 3) stays 30 Hz. */
    {
        static const uint8_t drops[] = {2,2,2,2,2,2,2,3};
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, drops, sizeof(drops), 600) == ACE_RATE_K2, "30 Hz with drops: %s",
              ace_rate_name(d.stable));
    }
    /* Two flips in one slot (0) and then a gap of 2: a jittery 60 Hz game, not 30. */
    {
        static const uint8_t jitter[] = {1,1,1,1,1,1,1,0,2,1,1,1,1,1,1,1};
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, jitter, sizeof(jitter), 600) == ACE_RATE_K1,
              "two flips per slot read 60 Hz: %s", ace_rate_name(d.stable));
    }
    /* 1, 2, 1, 2: no one rate. */
    {
        static const uint8_t alt[] = {1,2};
        ace_det_reset(&d);
        vc = 1000;
        CHECK(det_run(&d, &vc, alt, sizeof(alt), 600) == ACE_RATE_VARIABLE, "1,2 alternating: %s",
              ace_rate_name(d.stable));
    }
    /* Exit timing: 30 Hz, then 60 Hz. The class is left 0.5 s into the disagreement and 60 Hz
     * entered once it has agreed for 1.0 s. */
    {
        ace_det_reset(&d);
        vc = 1000;
        (void)det_run(&d, &vc, k2, 1, 400);
        uint32_t change = vc, left = 0, entered = 0;
        for (int i = 0; i < 400 && !entered; i++) {
            vc += 1;
            (void)ace_det_flip(&d, vc, 1);
            if (!left && d.stable != ACE_RATE_K2) left = vc;
            if (d.stable == ACE_RATE_K1) entered = vc;
        }
        CHECK(left && left - change <= 32u + ACE_DET_LEAVE_VBLANKS + 2u, "30 Hz left after %u VBLANKs",
              left - change);
        CHECK(entered && d.ever_stable && d.last_rate == ACE_RATE_K1, "60 Hz entered after %u VBLANKs",
              entered - change);
    }
    /* Hitches the class rule absorbs (two short intervals, one long one) never move it. */
    {
        static const uint8_t hitch[] = {1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,6,2,2,2,2,2,2,2,2,2,2,2,
                                        2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
        ace_det_reset(&d);
        vc = 1000;
        (void)det_run(&d, &vc, k2, 1, 400);
        int changed = 0;
        for (size_t i = 0; i < sizeof(hitch); i++) {
            vc += hitch[i];
            changed |= ace_det_flip(&d, vc, 1);
        }
        CHECK(!changed && d.stable == ACE_RATE_K2, "hitches keep 30 Hz: %s", ace_rate_name(d.stable));
    }
    /* STATIC: 30 VBLANKs with no new frame, then frames at the old rate return to it at once. */
    {
        ace_det_reset(&d);
        vc = 1000;
        (void)det_run(&d, &vc, k2, 1, 400);
        uint32_t last = vc;
        for (int i = 1; i <= 40; i++) (void)ace_det_flip(&d, last + (uint32_t)i, 0);
        CHECK(d.stable == ACE_RATE_STATIC && d.last_rate == ACE_RATE_K2, "static keeps the class: %s",
              ace_rate_name(d.stable));
        vc = last + 40;
        vc += 2;
        (void)ace_det_flip(&d, vc, 1);
        CHECK(d.stable == ACE_RATE_K2, "frames at the kept rate return to it: %s", ace_rate_name(d.stable));
    }
}

/* C4's feed: a flip is new when its target was drawn since its own previous flip, and two
 * flips of the same slot are both seen (the feed sits before the presenter's slot rule). */
static void cell_feed(void) {
    AceReadoutView v;
    (void)ace_init_spec(k_good, NULL);
    uint64_t ns = 0;
    /* Single buffer, 30 Hz: drawn every other VBLANK, flipped every VBLANK. */
    for (uint32_t vc = 1; vc <= 600; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        ace_note_host_wait(8000000u);
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    CHECK(ace_rate_stable() == ACE_RATE_K2, "single-buffered 30 Hz: %s", ace_rate_name(ace_rate_stable()));
    CHECK(ace_readout_view(&v) && strcmp(v.text, "100%  30 FPS") == 0, "readout '%s'", v.text);
    /* Double buffer, 60 Hz with every other flip landing in the slot of the one before. */
    (void)ace_init_spec(k_good, NULL);
    ns = 0;
    uint32_t vc = 1;
    for (int i = 0; i < 600; i++) {
        uint32_t fb = (i & 1) ? 0x88000u : 0x0u;
        ace_note_target_draw(fb);
        if (i % 16 == 7) { /* two flips share a slot, the next slot has none */ }
        else vc++;
        ns += 16683333u;
        (void)ace_flip(fb, 0x22000u, vc, ns);
    }
    CHECK(ace_rate_stable() == ACE_RATE_K1, "double-buffered 60 Hz with shared slots: %s",
          ace_rate_name(ace_rate_stable()));
    /* A latched flip of a target whose draws are not submitted yet is still new: the count is
     * taken when the draw is recorded, not when it is submitted. */
    (void)ace_init_spec(k_good, NULL);
    CHECK(ace_flip(0x44000u, 0x22000u, 1u, 0u) == 1, "a first flip is new");
    CHECK(ace_flip(0x44000u, 0x22000u, 2u, 0u) == 0, "an undrawn re-flip is not new");
    ace_note_target_draw(0x44000u);
    CHECK(ace_flip(0x44000u, 0x22000u, 3u, 0u) == 1, "a recorded draw makes the next flip new");
    ace_note_vram_write(0x50000u, 64u);
    CHECK(ace_flip(0x44000u, 0x22000u, 4u, 0u) == 1, "a CPU write inside the buffer makes it new");
    ace_note_vram_write(0x66000u, 64u);
    CHECK(ace_flip(0x44000u, 0x22000u, 5u, 0u) == 0, "a CPU write past the buffer does not");
    AceCounters n;
    ace_counters(&n);
    CHECK(n.flips == 5 && n.new_frames == 3 && n.draws == 2, "counters %llu %llu %llu",
          (unsigned long long)n.flips, (unsigned long long)n.new_frames, (unsigned long long)n.draws);
}

/* ---- C5: the speed readout ------------------------------------------------------------------ */

static AceWindow window(double fps, double idle, uint32_t skipped, AceRate stable, int ever, AceRate last) {
    AceWindow w;
    w.wall_ns = 1000000000ull;
    w.new_frames = (uint32_t)(fps + 0.5);
    w.idle_ns = (uint64_t)(idle * 1e9);
    w.skipped_slots = skipped;
    w.stable = stable;
    w.ever_stable = ever;
    w.last_rate = last;
    return w;
}

static void cell_readout(void) {
    AceReadoutState s;
    AceReadoutView v;
    AceWindow w;

    ace_readout_reset(&s);
    w = window(30, 0.5, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited && v.speed_pct == 100 && strcmp(v.text, "100%  30 FPS") == 0, "30 on time: '%s'", v.text);

    /* A game that slows down on its own while the PC idles is at full speed, as on a PSP. */
    ace_readout_reset(&s);
    w = window(20, 0.6, 0, ACE_RATE_VARIABLE, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited && strcmp(v.text, "100%  20 FPS") == 0, "slow game, idle PC: '%s'", v.text);

    /* A steady slowdown keeps T: the detector moves to VARIABLE and then to 20 Hz, the target
     * stays 30. */
    ace_readout_reset(&s);
    w = window(26, 0.0, 4, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 86 &&
          strcmp(v.text, "\xE2\x96\xB2 86%  26 FPS  PC LIMITED") == 0, "slowdown begins: '%s'", v.text);
    w = window(20, 0.0, 10, ACE_RATE_K3, 1, ACE_RATE_K3);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 66 && v.target_fps > 29.9 && v.target_fps < 30.0,
          "steady slowdown keeps T: '%s' T=%.2f", v.text, v.target_fps);
    /* A busy PC with no skipped slot is limited when frames come slower than the class allows. */
    ace_readout_reset(&s);
    w = window(27, 0.01, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 90, "busy PC, no skip: '%s'", v.text);
    /* ...and not when they come on time. */
    ace_readout_reset(&s);
    w = window(30, 0.01, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited, "busy PC on time: '%s'", v.text);

    /* 60 FPS menus, 30 FPS play: one skipped slot in play is judged against 30, not 60. */
    ace_readout_reset(&s);
    w = window(60, 0.4, 0, ACE_RATE_K1, 1, ACE_RATE_K1);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.pc_limited && strcmp(v.text, "100%  60 FPS") == 0, "menus at 60: '%s'", v.text);
    w = window(30, 0.4, 0, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    w = window(29, 0.3, 1, ACE_RATE_K2, 1, ACE_RATE_K2);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && v.speed_pct == 96 && v.target_fps < 30.0,
          "one skipped slot in play: '%s' T=%.2f", v.text, v.target_fps);

    /* Before a class has been stable once, no percentage. */
    ace_readout_reset(&s);
    w = window(27, 0.5, 0, ACE_RATE_NONE, 0, ACE_RATE_NONE);
    ace_readout_judge(&s, &w, &v);
    CHECK(!v.have_target && strcmp(v.text, "\xE2\x80\x94 27 FPS") == 0, "no class yet: '%s'", v.text);
    w = window(20, 0.0, 3, ACE_RATE_NONE, 0, ACE_RATE_NONE);
    ace_readout_judge(&s, &w, &v);
    CHECK(v.pc_limited && !v.have_target &&
          strcmp(v.text, "\xE2\x96\xB2 \xE2\x80\x94  20 FPS  PC LIMITED") == 0, "limited, no class: '%s'", v.text);
}

/* ---- the per-run report --------------------------------------------------------------------- */

static void cell_report(const char *dir) {
    char json[ACE_REPORT_MAX];
    (void)ace_init_spec(k_good, NULL);
    ace_set_af_effective(8, NULL);
    ace_feature_unavailable(ACE_FEATURE_TEXT, "no \"window\" \\ here");
    uint64_t ns = 0;
    for (uint32_t vc = 1; vc <= 300; vc++) {
        if (vc % 2 == 0) ace_note_target_draw(0x0u);
        ns += 16683333u;
        (void)ace_flip(0x0u, 0x22000u, vc, ns);
    }
    size_t len = ace_report_json(json, sizeof(json));
    CHECK(len > 0 && len < ACE_REPORT_MAX, "report length %zu", len);
    CHECK(strstr(json, "\"schema\": 1") && strstr(json, "\"af\": 8") &&
          strstr(json, "\"af_device_max\": 8") && strstr(json, "\"dominant\": \"30 Hz\"") &&
          strstr(json, "\"text\": \"no window  here\""), "report content:\n%s", json);
    CHECK(ace_report_json(json, 64) == 0 && json[0] == '\0', "a short buffer refuses the report");

    /* ace_finish writes it through a temporary file and replaces an older one. */
    char path[512];
    snprintf(path, sizeof(path), "%s%sace_selftest_report.json", dir ? dir : "", dir ? "/" : "");
    FILE *old = fopen(path, "wb");
    if (old) { fputs("stale", old); fclose(old); }
    (void)ace_init_spec(k_good, path);
    for (uint32_t vc = 1; vc <= 100; vc++) { ace_note_target_draw(0x0u); (void)ace_flip(0x0u, 0x22000u, vc, vc * 16683333ull); }
    ace_finish();
    FILE *f = fopen(path, "rb");
    char back[ACE_REPORT_MAX] = {0};
    size_t got = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f) fclose(f);
    CHECK(got > 0 && strncmp(back, "{\n  \"schema\": 1", 15) == 0, "report file replaced (%zu bytes)", got);
    (void)remove(path);
    /* An off run writes nothing. */
    (void)ace_init_spec("off", path);
    ace_finish();
    f = fopen(path, "rb");
    CHECK(f == NULL, "an off run wrote a report");
    if (f) fclose(f);
}

int main(int argc, char **argv) {
    cell_parse();
    cell_switch();
    cell_detector();
    cell_feed();
    cell_readout();
    cell_report(argc > 1 ? argv[1] : NULL);
    if (s_fail) {
        fprintf(stderr, "ace-selftest: %d check(s) FAILED\n", s_fail);
        return 1;
    }
    printf("ace-selftest: PASS (parse, switch, detector, feed, readout, report)\n");
    return 0;
}
