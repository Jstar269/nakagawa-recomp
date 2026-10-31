// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Input: sceCtrl sampling and sceHprm. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* Read on every controller sample and every presented frame. */
int hle_inlog_on(void) {
    static int enabled = -1;
    return hle_env_switch(&enabled, "SR_INLOG");
}
/* Retained controller sampling state (TD-24 batch 4): the Set calls store
 * their arguments and the getter below reports the stored pair. Public
 * behaviour reference: PSPSDK pspctrl.h (sceCtrlSetSamplingMode/Cycle/
 * IdleCancelThreshold) and PPSSPP Core/HLE/sceCtrl.cpp, where the sets update
 * the sampling state later reads observe. The power-on defaults are digital
 * mode (0), cycle 0, and disabled thresholds (-1/-1, matching the previous
 * fixed getter and its PPSSPP-default comment). The setters validate their
 * argument (issue #1390): the accepted ranges and the two error codes are
 * pinned by PSPAutotests tests/ctrl/sampling and tests/ctrl/sampling2 (.expected
 * files recorded on a PSP), corroborated by PPSSPP Core/HLE/sceCtrl.cpp; there
 * is no project oracle case for them yet. The mode decides whether a sample carries
 * the stick's position, and the cycle how often samples are taken (sr_ctrl_sample,
 * ctrl_cycle_set). */
#define SCE_CTRL_ERROR_INVALID_MODE 0x80000107u  /* SCE_KERNEL_ERROR_INVALID_MODE */
#define SCE_CTRL_ERROR_INVALID_VALUE 0x800001feu /* SCE_KERNEL_ERROR_INVALID_VALUE */
uint32_t s_ctrl_sampling_mode = 0u;
uint32_t s_ctrl_sampling_cycle = 0u;
static uint32_t s_ctrl_idle_reset = 0xFFFFFFFFu;
static uint32_t s_ctrl_idle_back = 0xFFFFFFFFu;
static void ctrl_cycle_set(uint32_t cycle);
/* sceHprmIsRemoteExist(void): 1 when the infrared remote is plugged in, else 0 (PSPSDK
 * psphprm.h). The runtime models the accessory as absent by default, so the answer comes from that
 * modeled state and is 0 until a model attaches a remote. The state is a model, not a measurement. */
static int s_hprm_remote_attached = 0;
static uint32_t h_HprmIsRemoteExist(CpuState *s) {
    (void)s;
    return s_hprm_remote_attached ? 1u : 0u;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only setter so the executable harness can check that the answer follows the model. */
void sr_hle_test_hprm_set_remote(int attached) { s_hprm_remote_attached = attached != 0; }
#endif

/* sceHprmIsHeadphoneExist(void): 1 when headphones are plugged in, else 0 (PSPSDK psphprm.h). Measured
 * 0 on the PSP-3000 oracle with plain headphones plugged in (2026-10-10, hle-hprm), so plain headphones
 * do not make this 1; the runtime models the accessory as absent by default. */
static int s_hprm_headphone_attached = 0;
static uint32_t h_HprmIsHeadphoneExist(CpuState *s) {
    (void)s;
    return s_hprm_headphone_attached ? 1u : 0u;
}

#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_hprm_set_headphone(int attached) { s_hprm_headphone_attached = attached != 0; }
#endif

/* sceHprmIsMicrophoneExist(void): 1 when a microphone is present, else 0 (PSPSDK psphprm.h). Measured
 * 1 on the PSP-3000 oracle (built-in microphone; 2026-10-10, hle-hprm, with and without headphones),
 * so the default follows the oracle model. */
static int s_hprm_microphone_attached = 1;
static uint32_t h_HprmIsMicrophoneExist(CpuState *s) {
    (void)s;
    return s_hprm_microphone_attached ? 1u : 0u;
}

#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_hprm_set_microphone(int attached) { s_hprm_microphone_attached = attached != 0; }
#endif

/* sceCtrlGetIdleCancelThreshold(int *idlereset, int *idleback): report the
 * stored thresholds (power-on default: both "disabled", -1). */
static uint32_t h_CtrlGetIdleCancelThreshold(CpuState *s) {
    if (A0) MEM_W32(A0, s_ctrl_idle_reset);
    if (A1) MEM_W32(A1, s_ctrl_idle_back);
    return 0;
}
/* sceCtrlSetSamplingMode(mode): store the mode (0 digital, 1 analog), returning
 * the previous one. Any other mode, 2 and -1 included, is refused with
 * INVALID_MODE and leaves the stored mode unchanged (PSPAutotests ctrl/sampling).
 * The whole register is range-checked; whether firmware first truncates the
 * argument to 8 bits (uOFW src/kd/ctrl/ctrl.c reconstructs a u8 parameter) is
 * UNMEASURED, so a value such as 0x101 is refused here. */
static uint32_t h_CtrlSetSamplingMode(CpuState *s) {
    if (A0 > 1u) return SCE_CTRL_ERROR_INVALID_MODE;
    uint32_t prev = s_ctrl_sampling_mode;
    s_ctrl_sampling_mode = A0;
    return prev;
}
/* sceCtrlSetSamplingCycle(cycle): store the cycle in microseconds, returning the
 * previous one. 0 (sample on VBLANK) and 5555..20000 are accepted; 1..5554, values
 * above 20000 and -1 are refused with INVALID_VALUE and leave the stored cycle
 * unchanged (PSPAutotests ctrl/sampling and ctrl/sampling2). */
static uint32_t h_CtrlSetSamplingCycle(CpuState *s) {
    if ((A0 > 0u && A0 < 5555u) || A0 > 20000u) return SCE_CTRL_ERROR_INVALID_VALUE;
    uint32_t prev = s_ctrl_sampling_cycle;
    ctrl_cycle_set(A0);
    return prev;
}
/* sceCtrlSetIdleCancelThreshold(idlereset, idleback): store both thresholds.
 * Returns success; the stored pair is observable through
 * sceCtrlGetIdleCancelThreshold, which is the round-trip the regression pins
 * (whether firmware returns a previous value here is UNMEASURED). */
static uint32_t h_CtrlSetIdleCancelThreshold(CpuState *s) {
    s_ctrl_idle_reset = A0;
    s_ctrl_idle_back = A1;
    return 0;
}

/* sceCtrlGetSamplingMode(int *pmode): report the mode retained by
 * sceCtrlSetSamplingMode (power-on default digital, 0) through the required
 * out-pointer, and return 0 as the PSPSDK pspctrl.h contract documents. The span
 * is validated before anything is written, so a refused call leaves guest memory
 * untouched; the refusal code is the same ILLEGAL_ADDR used by the clock getters.
 * The pointer-error code itself is UNMEASURED on hardware. */
static uint32_t h_CtrlGetSamplingMode(CpuState *s) {
    (void)s;
    if (!A0 || !sr_guest_span_writable(A0, 4u)) return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    MEM_W32(A0, s_ctrl_sampling_mode);
    return 0;
}
/* sceCtrlGetSamplingCycle(int *pcycle): report the cycle retained by
 * sceCtrlSetSamplingCycle (power-on default 0) and return 0, the round trip
 * PSPAutotests ctrl/sampling records. The out-pointer is checked like the mode
 * getter above. PSPAutotests leaves the NULL call out because it crashes the PSP,
 * so there is no PSP return code to match: the ILLEGAL_ADDR refusal is this
 * runtime's fail-closed choice, not a measured code. */
static uint32_t h_CtrlGetSamplingCycle(CpuState *s) {
    (void)s;
    if (!A0 || !sr_guest_span_writable(A0, 4u)) return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    MEM_W32(A0, s_ctrl_sampling_cycle);
    return 0;
}
/* PSP controller ring buffer, modelled on PPSSPP (Core/HLE/sceCtrl.cpp): one sample is latched
 * per VBLANK into a 64-entry ring; sceCtrlReadBufferPositive returns the samples accumulated
 * since the last read (real per-frame history), not a flat copy of the current state. Games do
 * edge detection across these samples, so a flat fill makes a button look permanently held and a
 * press is never seen -- which is why the title/attract loop never advanced on START. */
#define CTRL_RING 64
/* One guest SceCtrlData record: timestamp, button field, two analog axes, then
 * reserved bytes this runtime does not populate. */
#define CTRL_SAMPLE_BYTES 16u
/* A request larger than the sample ring is refused rather than clamped.
 * CORROBORATIVE_ONLY: PPSSPP Core/HLE/sceCtrl.cpp __CtrlReadBuffer() returns
 * SCE_KERNEL_ERROR_INVALID_SIZE for nBufs > NUM_CTRL_BUFFERS. Not measured here. */
#define SCE_CTRL_ERROR_INVALID_SIZE 0x80000104u
static CtrlSample s_ctrl_ring[CTRL_RING] = { [0 ... CTRL_RING-1] = { 0, 0, 128, 128, 0 } };
static int s_ctrl_w = 1, s_ctrl_r = 0;   /* start with one sample available */
/* The latch cursor is independent of the ReadBuffer cursor s_ctrl_r: sceCtrlReadLatch reports the
 * transitions between the sampling cycles since its previous read, so it keeps the button state it
 * last saw and the sample count at that read. s_ctrl_sample_count counts every sampling cycle. */
static uint32_t s_ctrl_sample_count = 0u;
static uint32_t s_latch_prev_btn = 0u;
static uint32_t s_latch_seen_count = 0u;

/* The input the next sample carries: buttons, stick and scripted-input id, latched once per
 * VBLANK by sr_ctrl_sample() (the host's input is polled once per frame). */
CtrlSample s_ctrl_live = { 0, 0, 128, 128, 0 };
/* Sampling cycle (#1390): with cycle 0 the controller is sampled once per VBLANK. With a cycle
 * of 5555 to 20000 us it is sampled every `cycle` microseconds of guest time instead, and a
 * VBLANK adds no sample: pspautotests tests/ctrl/sampling2 at f573fe8c (recorded on a PSP)
 * counts 3 samples per VBLANK at 5555, 2 at 9999 and 1 at 16666 and at 20000. Those samples
 * are latched lazily: whatever looks at the ring first latches every sample due by the current
 * guest time, each stamped at its own cycle boundary and carrying s_ctrl_live. A blocked read
 * sleeps until the next boundary. s_ctrl_cycle_next_us is that boundary, 0 with cycle 0. */
static uint64_t s_ctrl_cycle_next_us;

void ctrl_push(const CtrlSample *smp, uint32_t ts) {
    s_ctrl_ring[s_ctrl_w] = *smp;
    s_ctrl_ring[s_ctrl_w].ts = ts;
    s_ctrl_w = (s_ctrl_w + 1) % CTRL_RING;
    if (s_ctrl_w == s_ctrl_r) s_ctrl_r = (s_ctrl_r + 1) % CTRL_RING;  /* drop oldest on overflow */
    s_ctrl_sample_count++;
}

/* Latch every cycle sample due by now, and wake the blocked readers when one was latched. */
void ctrl_cycle_catch_up(void) {
    if (!s_ctrl_cycle_next_us || !s_ctrl_sampling_cycle) return;
    uint64_t now = sched_vtime_us();
    if (s_ctrl_cycle_next_us > now) return;
    uint64_t due = (now - s_ctrl_cycle_next_us) / s_ctrl_sampling_cycle + 1u;
    /* Only the newest CTRL_RING samples can still be in the ring: the older ones are counted
     * (sceCtrlReadLatch reports every cycle since its last read) but never written. */
    if (due > CTRL_RING) {
        uint64_t skipped = due - CTRL_RING;
        s_ctrl_sample_count += (uint32_t)skipped;
        s_ctrl_cycle_next_us += skipped * s_ctrl_sampling_cycle;
        due = CTRL_RING;
    }
    for (uint64_t i = 0; i < due; i++) {
        ctrl_push(&s_ctrl_live, (uint32_t)s_ctrl_cycle_next_us);
        s_ctrl_cycle_next_us += s_ctrl_sampling_cycle;
    }
    sched_wake(CTRL_WAIT_OBJ);
}

/* sceCtrlSetSamplingCycle's effect: the samples due under the old cycle are latched first, then
 * the first sample of a non-zero cycle is one cycle from now. When on the PSP the first timer
 * sample follows the call is UNMEASURED; the spacing after it is measured. */
static void ctrl_cycle_set(uint32_t cycle) {
    ctrl_cycle_catch_up();
    s_ctrl_sampling_cycle = cycle;
    s_ctrl_cycle_next_us = cycle ? sched_vtime_us() + cycle : 0u;
}

/* Every scripted input source -- the legacy table below, the route program's press steps and
 * the auto-START pulse -- delivers through this one player in guest time (scripted_input.h):
 * each pressed and released stretch lasts at least its authored number of controller samples
 * AND until the guest has read a sample of it, so a host that falls behind stretches a script
 * but can never shorten or skip a press. Only one source is active in a run. */
SrInputPlayer  s_input;
#ifdef SR_HLE_THREAD_SELFTEST
/* TD-24 batch 4: isolate the retained sampling fixtures from one another.
 * Test builds only; restores the power-on defaults, never production state. */
void sr_hle_test_ctrl_sampling_reset(void) {
    s_ctrl_sampling_mode = 0u;
    s_ctrl_sampling_cycle = 0u;
    s_ctrl_idle_reset = 0xFFFFFFFFu;
    s_ctrl_idle_back = 0xFFFFFFFFu;
    s_ctrl_cycle_next_us = 0u;
}
#endif
/* sceCtrlReadBuffer*: fill SceCtrlData[count]. Positive reports pressed buttons as set bits;
 * Negative reports them inverted (set = not pressed), so it must write ~buttons -- writing the
 * positive mask there makes the game see almost every button held and run wild (it jumped through
 * an uninitialised menu handler). */
/* Latch one controller sample per frame into the ring (called from sr_vblank_tick). */
int sr_pause_script_input_pending(uint32_t target_vcount) {
    if (!s_input.current || s_input.seg.mask == 0u ||
        s_input.seg.not_before > target_vcount || sr_input_was_read(&s_input)) return 0;
    return (s_input.seg.mask & 0x4000u) ? 2 : 1;
}

/* Fill SceCtrlData[n] from the ring. Returns the number of new samples since the last read (the
 * PSP semantics), giving the game genuine per-frame history for edge/latch detection. Negative
 * reports inverted buttons. peek does not consume or block. */
static uint32_t ctrl_fill_n(uint32_t buf, uint32_t nbufs, int negate, int peek) {
    /* Contract order: an out-of-contract request is refused before the ring is
     * consulted, so a rejected call can neither consume history nor write a byte.
     * Clamping an oversized request the way this used to is a fabricated success:
     * the guest asked for something the API cannot do and was told it worked. */
    if (nbufs > CTRL_RING) return SCE_CTRL_ERROR_INVALID_SIZE;
    /* A zero count is refused with the same code (issue #1390 acceptance, from the
     * project's PSP-3000 measurement cited there; not yet a public oracle cell,
     * which #311 tracks for PSP-SYSTEM-001). PPSSPP and the uOFW reconstruction
     * return 0 here instead. Refusing writes nothing and consumes nothing. */
    if (nbufs == 0) return SCE_CTRL_ERROR_INVALID_SIZE;
    ctrl_cycle_catch_up();
    int avail;
    if (peek) {
        /* A peek reports the newest `nbufs` samples, read before or not, and never blocks:
         * pspautotests tests/ctrl/sampling and ctrl/vblank at f573fe8c (recorded on a PSP)
         * answer 64 to a peek of 64, also right after a read and inside a VBLANK handler. */
        avail = (int)nbufs;
    } else {
        /* A read returns the samples latched since the previous read, the newest `nbufs` of
         * them, and consumes all of them (uOFW _sceCtrlReadBuf and PPSSPP __CtrlReadBuffer;
         * PSP-KERNEL-SEM-001 ctrl-short-read). With none it waits for the next sample, and
         * returns it: a read right after a read answers 1 (pspautotests ctrl/sampling and
         * ctrl/vblank). A read that would wait answers ILLEGAL_CONTEXT in an interrupt handler
         * and CAN_NOT_WAIT with interrupts or dispatch disabled (pspautotests
         * tests/intr/waits.expected L222, L223 and L390). */
        avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
        if (avail == 0) {
            if (sched_is_intr_context()) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
            if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
        }
        while (avail == 0) {
            if (s_ctrl_cycle_next_us) {
                uint64_t now = sched_vtime_us();
                uint64_t wait = s_ctrl_cycle_next_us > now ? s_ctrl_cycle_next_us - now : 1u;
                (void)sched_block_on_timeout(CTRL_WAIT_OBJ, (uint32_t)wait);
                ctrl_cycle_catch_up();
            } else {
                sched_block_on(CTRL_WAIT_OBJ);   /* sr_ctrl_sample() wakes it at the next VBLANK */
            }
            avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
        }
        if (avail > (int)nbufs) avail = (int)nbufs;
    }
    /* Whole-span preflight with checked arithmetic. The scalar accessors reject an
     * out-of-range store one word at a time, so without this a partially valid
     * destination gets part of the history written -- and a base near the top of
     * the address space wraps buf + i*16 down into an unrelated but IN-range guest
     * address -- while the return value still claims every sample landed.
     *
     * avail is bounded by CTRL_RING above, so the multiply cannot currently wrap and
     * that leg has NO failing-before -- it is defence in depth against a later ring
     * resize, not a repaired defect. The span check itself does have one. */
    uint32_t span = 0;
    if (!sr_size_mul_ok((uint32_t)avail, CTRL_SAMPLE_BYTES, &span) ||
        !sr_guest_span_writable(buf, span))
        return 0;
    /* Oldest of the delivered samples first; peek differs only in blocking and in
     * whether the read cursor is advanced below, not in where the window starts. */
    int start = (s_ctrl_w - avail + CTRL_RING) % CTRL_RING;
    uint32_t newest_input = 0u;
    for (int i = 0; i < avail; i++) {
        CtrlSample smp = s_ctrl_ring[(start + i) % CTRL_RING];
        if (smp.input_id > newest_input) newest_input = smp.input_id;
        uint32_t field = negate ? ~smp.btn : smp.btn;   /* negative mode inverts buttons only */
        uint32_t e = buf + (uint32_t)i * CTRL_SAMPLE_BYTES;
        MEM_W32(e + 0, smp.ts);        /* stamped when the sample was latched */
        MEM_W32(e + 4, field);
        MEM_W8(e + 8, smp.lx); MEM_W8(e + 9, smp.ly); MEM_W8(e + 10, 128); MEM_W8(e + 11, 128);
    }
    if (!peek) s_ctrl_r = s_ctrl_w;                /* consume */
    /* Scripted input advances in guest time: this read is what tells the script the guest
     * has seen the state it is holding (scripted_input.h). */
    if (sr_input_read(&s_input, newest_input) && hle_inlog_on())
        fprintf(stderr, "ctrl_read: vcount=%u guest read scripted 0x%04x (latched %u samples "
                        "since vblank %u)\n",
                s_vcount_fwd, s_input.seg.mask, s_input.latched, s_input.since);
    if (hle_inlog_on()) {
        static unsigned long calls = 0;
        CtrlSample latest = s_ctrl_ring[(s_ctrl_w - 1 + CTRL_RING) % CTRL_RING];
        if ((++calls % 200) == 0 || (latest.btn & 0x8))
            fprintf(stderr, "ctrl_fill #%lu vc=%u avail=%d latest=0x%x lx=%u ly=%u buf=0x%08x neg=%d peek=%d\n",
                    calls, s_vcount_fwd, avail, latest.btn, latest.lx, latest.ly, buf, negate, peek);
    }
    return (uint32_t)avail;
}
static uint32_t ctrl_fill(uint32_t buf, uint32_t count, int negate) {
    return ctrl_fill_n(buf, count, negate, 0);
}
static uint32_t h_CtrlReadBuffer(CpuState *s) { return ctrl_fill(A0, A1, 0); }
static uint32_t h_CtrlPeekBufferPositive(CpuState *s) {
    return ctrl_fill_n(A0, A1, 0, 1);
}
/* sceCtrlReadBufferNegative / sceCtrlPeekBufferNegative: the positive contract with
 * the button field inverted (PSPSDK pspctrl.h); the read consumes and the peek does not. */
static uint32_t h_CtrlReadBufferNegative(CpuState *s) { return ctrl_fill(A0, A1, 1); }
static uint32_t h_CtrlPeekBufferNegative(CpuState *s) {
    return ctrl_fill_n(A0, A1, 1, 1);
}

/* sceCtrlReadLatch(SceCtrlLatch *latch) (PSPSDK pspctrl.h, 16 bytes: uiMake, uiBreak, uiPress,
 * uiRelease): uiMake = buttons that transitioned to pressed, uiBreak = buttons that transitioned to
 * released, uiPress = buttons in the pressed state, uiRelease = buttons in the released state, all
 * across the sampling cycles since the previous latch read. The return is the number of sampling
 * cycles since that read (PSPSDK). The complement of the button field is the released state, the
 * same 32-bit convention the negative buffer read uses. Samples overwritten before a latch read are
 * not reported one by one: the ring keeps the latest CTRL_RING - 1 samples, and a transition inside
 * an overwritten span is lost. */
/* One latch computation for sceCtrlReadLatch (consume = 1) and sceCtrlPeekLatch (consume = 0). A read
 * advances the latch cursor, so the transitions it reported are consumed. A peek writes the same words
 * and leaves the cursor alone, so a pending transition is still reported by the next read. PSP-3000 6.61
 * oracle 2026-10-15, record PSP-HLE-CTRL-LATCH-001 (cases latch-peek-initial, latch-peek-after-poll): the
 * no-input words are make/break/press 0 and release 0xFFFFFFFF. The clear-or-keep distinction is not
 * measured there (the release field stays saturated). */
static uint32_t ctrl_latch_compute(uint32_t out, int consume) {
    ctrl_cycle_catch_up();
    uint32_t produced = s_ctrl_sample_count - s_latch_seen_count;
    uint32_t retained = produced < (uint32_t)(CTRL_RING - 1) ? produced : (uint32_t)(CTRL_RING - 1);
    uint32_t prev = s_latch_prev_btn, cur = prev, make = 0u, brk = 0u;
    if (!out || !sr_guest_span_writable(out, 16u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (retained) {
        int start = (s_ctrl_w - (int)retained + CTRL_RING) % CTRL_RING;
        for (uint32_t i = 0; i < retained; i++) {
            cur = s_ctrl_ring[(start + (int)i) % CTRL_RING].btn;
            make |= cur & ~prev;
            brk |= prev & ~cur;
            prev = cur;
        }
    }
    if (consume) {
        s_latch_prev_btn = cur;
        s_latch_seen_count = s_ctrl_sample_count;
    }
    MEM_W32(out + 0, make);
    MEM_W32(out + 4, brk);
    MEM_W32(out + 8, cur);
    MEM_W32(out + 12, ~cur);
    return produced;
}

static uint32_t h_CtrlReadLatch(CpuState *s) { (void)s; return ctrl_latch_compute(A0, 1); }
static uint32_t h_CtrlPeekLatch(CpuState *s) { (void)s; return ctrl_latch_compute(A0, 0); }

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only: push one sampling cycle with the given button field, as sr_ctrl_sample stores
 * it, without scripted input, the guest-time stamp or the wake. */
void sr_hle_test_ctrl_push_sample(uint32_t buttons) {
    const CtrlSample smp = { buttons, 0u, 128u, 128u, 0u };
    ctrl_push(&smp, 0u);
}

/* Test-build-only: the power-on ring (one neutral sample available, no history), so a test that
 * needs an empty or a known ring does not depend on what ran before it in the process. */
void sr_hle_test_ctrl_ring_reset(void) {
    for (int i = 0; i < CTRL_RING; i++) s_ctrl_ring[i] = (CtrlSample){ 0, 0, 128, 128, 0 };
    s_ctrl_w = 1;
    s_ctrl_r = 0;
    s_ctrl_sample_count = 0u;
    s_latch_prev_btn = 0u;
    s_latch_seen_count = 0u;
    s_ctrl_live = (CtrlSample){ 0, 0, 128, 128, 0 };
    s_ctrl_cycle_next_us = s_ctrl_sampling_cycle ? sched_vtime_us() + s_ctrl_sampling_cycle : 0u;
}

/* Test-build-only: start the latch from the current sample count and an all-released state. */
void sr_hle_test_ctrl_latch_reset(void) {
    s_latch_prev_btn = 0u;
    s_latch_seen_count = s_ctrl_sample_count;
}
#endif
#ifdef SR_HLE_THREAD_SELFTEST
/* The latch and read halves of sr_ctrl_sample() / ctrl_fill_n() for tests that drive
 * sr_route_step() directly: latch one sample of the current scripted state and, when
 * `guest_reads`, hand it to the guest. A route test that delivers every vblank is a guest
 * that polls every frame, which is the case where guest time and VCOUNT agree. */
void sr_input_test_deliver(int guest_reads) {
    uint32_t id = sr_input_latch(&s_input);
    if (guest_reads) (void)sr_input_read(&s_input, id);
}
#endif

/* TD-24 batch 4 shared families: one definition reached by both sr_hle_init()
 * branches, so the executable harness pins the production mapping through
 * dispatch (same rule as the batch-2 clock helpers above). */
void hle_register_ctrl_sampling_handlers(void) {
    sr_hle_register(0x208db1bd, "sceHprmIsRemoteExist", h_HprmIsRemoteExist);
    sr_hle_register(0x7e69eda4, "sceHprmIsHeadphoneExist", h_HprmIsHeadphoneExist);
    sr_hle_register(0x219c58f1, "sceHprmIsMicrophoneExist", h_HprmIsMicrophoneExist);
    sr_hle_register(0x1f4011e6, "sceCtrlSetSamplingMode", h_CtrlSetSamplingMode);
    sr_hle_register(0x6a2774f3, "sceCtrlSetSamplingCycle", h_CtrlSetSamplingCycle);
    sr_hle_register(0xa7144800, "sceCtrlSetIdleCancelThreshold", h_CtrlSetIdleCancelThreshold);
    sr_hle_register(0xda6b76a1, "sceCtrlGetSamplingMode", h_CtrlGetSamplingMode);
    sr_hle_register(0x02baad91, "sceCtrlGetSamplingCycle", h_CtrlGetSamplingCycle);
    /* 0x687660fa is GetIdleCancelThreshold(int*,int*), NOT ReadBufferNegative -- the pad
     * handler used pointer a1 as a buffer count and wrote up to a ring of SceCtrlData
     * through a1's 4-byte int (and could block the caller on the input ring). */
    sr_hle_register(0x687660fa, "sceCtrlGetIdleCancelThreshold", h_CtrlGetIdleCancelThreshold);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_input(void) {
    sr_hle_register(0x1f803938, "sceCtrlReadBufferPositive", h_CtrlReadBuffer);
    sr_hle_register(0x3a622550, "sceCtrlPeekBufferPositive", h_CtrlPeekBufferPositive);
    sr_hle_register(0x60b81f86, "sceCtrlReadBufferNegative", h_CtrlReadBufferNegative);
    sr_hle_register(0xc152080a, "sceCtrlPeekBufferNegative", h_CtrlPeekBufferNegative);
    sr_hle_register(0x0b588501, "sceCtrlReadLatch", h_CtrlReadLatch);
    sr_hle_register(0xb1d0e5cd, "sceCtrlPeekLatch", h_CtrlPeekLatch);
}
