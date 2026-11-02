// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The explicit stub set for the focused media build (#307): `make psmf-media-focused-selftest`.
 *
 * That target links tests/native/psmf_media_selftest.c against six HLE files only: hle_mpeg.c
 * (the subsystem under test) and the presentation boundary its A/V measurement drives
 * through the production handlers: hle_display.c, hle_audio.c, hle_audio_codec.c, hle_route.c
 * and hle_input.c. The other twenty HLE files, hle.c among them, are not linked. This file
 * supplies what those six call into the rest of HLE for, and nothing else.
 *
 * Three kinds of definition, each marked below:
 *   - Registry/dispatch/uid pool: hle.c owns the NID table, sr_syscall() and the kernel uid
 *     pool, and hle.c also composes every subsystem, so it cannot be linked without linking
 *     them all. The definitions here are the smallest forms of the same contract: register
 *     rejects a duplicate NID, dispatch runs the registered handler, an unregistered NID
 *     stops the run, and uids count up from 0x110. They are not a second implementation of
 *     the rest of sr_syscall() (flight recorder, logging, register poisoning); the full path
 *     stays covered by the existing selftests, which link all of HLE.
 *   - Answers for a subsystem that is absent: "no VBLANK handler is registered" is true when
 *     no kernel-interrupt code is linked, so the query returns the empty answer.
 *   - Everything else fails loudly: the call prints which subsystem owns the symbol and
 *     aborts. No stub reports success for work it did not do (AGENTS.md Section 6).
 *
 * tools/hle_focus_check.py proves the link is focused: it reads the symbol tables of the
 * linked executable's inputs and fails when any symbol of an excluded HLE file is defined in
 * it, or when a symbol this file stubs is defined by a linked HLE file too. */

#include "hle_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void focus_fail(const char *symbol, const char *owner) {
    fprintf(stderr, "HLE-FOCUS: %s (%s) was called in the focused media build; "
                    "the call is outside the subsystems this build links\n", symbol, owner);
    fflush(stderr);
    abort();
}

/* ---- registry, dispatch and uid pool (hle.c, hle_kernel_*) ---- */

enum { FOCUS_HLE_CAP = 256 };
static struct { uint32_t nid; const char *name; HleFn fn; uint32_t unsupported_error; } s_focus[FOCUS_HLE_CAP];
static int s_focus_n;
static int s_focus_ready;

static void focus_register(uint32_t nid, const char *name, HleFn fn, uint32_t error) {
    for (int i = 0; i < s_focus_n; i++) {
        if (s_focus[i].nid == nid) {
            fprintf(stderr, "HLE: duplicate NID 0x%08x (%s) rejected; first was %s\n",
                    nid, name, s_focus[i].name);
            return;
        }
    }
    if (s_focus_n >= FOCUS_HLE_CAP) focus_fail("sr_hle_register", "registry full");
    s_focus[s_focus_n].nid = nid;
    s_focus[s_focus_n].name = name;
    s_focus[s_focus_n].fn = fn;
    s_focus[s_focus_n].unsupported_error = error;
    s_focus_n++;
}

void sr_hle_register(uint32_t nid, const char *name, HleFn fn) {
    focus_register(nid, name, fn, 0u);
}

void sr_hle_register_unsupported(uint32_t nid, const char *name, uint32_t error) {
    if (!(error & 0x80000000u)) focus_fail("sr_hle_register_unsupported", "invalid error code");
    focus_register(nid, name, NULL, error);
}

/* The registrations hle.c's sr_hle_init() makes under SR_MPEG_MEDIA_SELFTEST, in that order. */
static void focus_init(void) {
    if (s_focus_ready) return;
    s_focus_ready = 1;
    hle_register_mpeg_ycbcr_handlers();
    hle_register_mpeg_avc_decode_handler();
    hle_register_psmf_player_handlers();
    hle_register_display_setframebuf_handler();
    hle_register_regular_audio_handlers();
    hle_register_output2_audio_handlers();
}

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    focus_init();
    for (int i = 0; i < s_focus_n; i++) {
        if (s_focus[i].nid != nid) continue;
        uint32_t ret = s_focus[i].unsupported_error ? s_focus[i].unsupported_error : s_focus[i].fn(s);
        s->r[2] = ret;
        return ret;
    }
    fprintf(stderr, "HLE-FOCUS: unimplemented nid 0x%08x in the focused media build\n", nid);
    fflush(stderr);
    abort();
}

static uint32_t s_focus_uid = 0x110;
uint32_t sr_alloc_uid(void) {
    while (s_focus_uid == 0u || s_focus_uid == SR_ROLE_UID_NONE) s_focus_uid++;
    return s_focus_uid++;
}

int guest_cstr(uint32_t addr, char *out, int max) {
    if (!out || max <= 0) return 0;
    out[0] = '\0';
    if (!addr) return 0;
    for (int i = 0; i < max; i++) {
        uint32_t cur = 0;
        if (!sr_size_add_ok(addr, (uint32_t)i, &cur) || !sr_guest_span_readable(cur, 1u)) {
            out[0] = '\0';
            return 0;
        }
        uint8_t c = MEM_R8(cur);
        if (!c) {
            out[i] = '\0';
            return 1;
        }
        out[i] = (char)c;
    }
    out[max - 1] = '\0';
    return 0;
}

/* hle.c's h_ok marker handler: success for a call that has nothing to do. Registered by the
 * display and audio runs for calls the build accepts and ignores. */
uint32_t h_ok(CpuState *s) { (void)s; return 0; }

/* ---- display timing mirror (hle.c, written by hle_route.c's vblank advance) ---- */

uint32_t s_vcount_fwd;
uint32_t sr_audio_vbl(void) { return s_vcount_fwd; }

/* ---- answers for subsystems that are not linked ---- */

int sr_vblank_subintr(uint32_t no, uint32_t *handler, uint32_t *arg) {
    (void)handler; (void)arg;
    return no >= 16u ? -1 : 0;   /* hle_kernel_interrupt.c: 16 sub-interrupts, none registered */
}
int sr_vblank_dispatch_registered(void) { return 0; }   /* hle_kernel_interrupt.c: none registered */
int sr_thread_has_pending_callbacks(uint32_t thread_uid) { (void)thread_uid; return 0; }
int sr_thread_dispatch_callbacks(void) { return 0; }
void sr_callback_unregister_owner(uint32_t thread_uid) { (void)thread_uid; }  /* none registered */
void sr_hle_release_thread_resources(uint32_t thread_uid) { (void)thread_uid; }  /* none held */
int sr_savedata_pause_busy(void) { return 0; }   /* hle_utility.c: no savedata dialog exists */
int sr_exit_request_stop_due(void) { return 0; }   /* hle_system.c: no quit request can be raised */
int ge_log_on(void) { return 0; }
int audio_stat_on(void) { return 0; }
uint32_t g_sdk_version;   /* hle.c: no compiled SDK version is set; 0 is "unknown" */

/* ---- everything else fails loudly ---- */

uint32_t sr_hle_resolve_late_import(uint32_t nid) {
    (void)nid; focus_fail("sr_hle_resolve_late_import", "hle.c late imports"); return 0;
}
void sr_dump_calls(void) { focus_fail("sr_dump_calls", "hle.c"); }
int hle_dispatch_guest_scoped(CpuState *s, uint32_t fn, const char *owner, uint32_t *fatal_target) {
    (void)s; (void)fn; (void)owner; (void)fatal_target;
    focus_fail("hle_dispatch_guest_scoped", "hle.c guest callbacks"); return 0;
}
uint64_t sr_savedata_guest_rtc_tick(void) { focus_fail("sr_savedata_guest_rtc_tick", "hle_rtc.c"); return 0; }
int32_t sr_savedata_console_offset_minutes(void) {
    focus_fail("sr_savedata_console_offset_minutes", "hle_rtc.c"); return 0;
}
int sr_prx_guest_write(uint32_t guest_addr, const void *src, uint32_t n) {
    (void)guest_addr; (void)src; (void)n; focus_fail("sr_prx_guest_write", "hle_kernel_module.c"); return 0;
}
int sr_hle_memstick_free_bytes(uint64_t *free_bytes) {
    (void)free_bytes; focus_fail("sr_hle_memstick_free_bytes", "hle_data.c"); return 0;
}
