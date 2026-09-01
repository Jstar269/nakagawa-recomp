// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Guest-memory frames for nested host->guest calls.
 *
 * A "nested host->guest call" is the runtime re-entering translated guest code
 * from inside an HLE handler: the GE signal/finish and list allocator/free
 * callbacks in src/rt/hle.c (ge_call_guest / ge_call_guest_rv) and the MPEG
 * ring-refill callback in src/rt/mpeg.c (call_guest3).  Each such call hands the
 * callee a guest stack pointer that is NOT the calling thread's own $sp.
 *
 * Before this module all of them used one fixed guest address, 0x09df8000.
 * Three properties of that arrangement were measured executably in
 * src/rt/hle_thread_selftest.c and are the reason this module exists:
 *
 *   1. Same thread, nested: an inner call overwrote the outer body's guest
 *      locals (test_nested_guest_call_abi).
 *   2. Two threads, each only one level deep: the marshalling installs no
 *      scheduler lock, so a nested call is preemptible at any SR_YIELD.  A
 *      second thread entering its own nested call destroyed the first thread's
 *      spilled locals across the preemption
 *      (test_nested_frames_isolate_concurrent_threads).
 *   3. 0x09df8000 was inside [SR_STACK_ARENA_FLOOR, SR_STACK_ARENA_CEIL), the
 *      descending arena sceKernelCreateThread carves guest thread stacks from.
 *      The fifth default-sized create landed a thread stack on top of it
 *      (test_nested_frame_region_is_reserved_from_thread_stacks).
 *
 * WHAT THIS MODULE IS.  A fixed, statically reserved guest region carved into
 * equal slots, handed out per (owner, depth) and returned on release, with
 * address-keyed guard bands either side of each slot's usable stack.  It owns
 * frame OWNERSHIP and ISOLATION only.  It deliberately does NOT change the
 * register-seeding convention of a nested call: the zeroed CpuState, the
 * inherited $gp, $ra = 0 and the 0xe4 VFPU prefix seeds are unchanged, and only
 * the value written to $sp comes from here.
 *
 * WHAT IT IS NOT.  This is not a PSP contract.  Whether hardware shares a stack
 * across nested guest calls, and what stack the PSP kernel hands a GE or MPEG
 * callback, remain NOT_ESTABLISHED and need a hardware probe.  This module
 * makes the runtime's own model coherent and non-destructive; it does not claim
 * the model is the console's.
 *
 * OWNER IDENTITY is the guest thread uid (sched_current_uid()).  0 means "no
 * current thread", i.e. interrupt/host context, which sr_yield() cannot switch
 * away from and which is therefore a single logical context of its own.
 *
 * CONCURRENCY.  This is a cooperative, single-host-thread runtime: guest threads
 * are coroutines and switch only at explicit scheduler boundaries.  Nothing here
 * is a mutex and nothing here is safe against true host-thread concurrency.
 */
#ifndef SR_NESTED_FRAMES_H
#define SR_NESTED_FRAMES_H

#include <stdint.h>

/* ---- reserved guest region -------------------------------------------------
 * [0x09f00000, 0x0a000000): 1 MiB between the top of the thread-stack arena and
 * the runtime's newlib heap arena at SR_HEAP_BASE (0x0a000008, src/rt/recomp.c).
 * This hole was already unclaimed, so reserving it moves no existing allocation:
 * thread stacks, the VBLANK interrupt stack, guest VRAM and the heap all keep the
 * addresses they had.  src/rt/sched.c derives SR_STACK_ARENA_CEIL from
 * SR_NESTED_FRAME_BASE so the exclusion is structural rather than a coincidence
 * of two literals that happen to agree today. */
#define SR_NESTED_FRAME_BASE      0x09f00000u
#define SR_NESTED_FRAME_END       0x0a000000u
#define SR_NESTED_FRAME_STRIDE    0x00010000u   /* 64 KiB per slot */
#define SR_NESTED_FRAME_SLOTS     16u           /* (END - BASE) / STRIDE */

/* Slot layout, relative to the slot base:
 *   [0x00000, 0x00200)  low guard
 *   [0x00200, 0x0fd00)  usable stack, 0xfb00 = 64256 bytes, grows DOWN from $sp
 *   [0x0fd00, 0x0fe00)  o32 incoming-argument save area above the initial $sp
 *   [0x0fe00, 0x10000)  high guard
 * $sp = base + 0xfd00, which is 16-byte aligned as the o32 ABI requires.
 * The previous arrangement gave a nested call the 0x8000 bytes between
 * 0x09df8000 and the VBLANK interrupt stack at 0x09df0000, and that budget was
 * implicit; 64256 bytes is strictly larger and is stated. */
#define SR_NESTED_FRAME_GUARD     0x00000200u
#define SR_NESTED_FRAME_SP_OFF    0x0000fd00u
#define SR_NESTED_FRAME_ARGSAVE   0x00000100u

/* Deterministic per-owner nesting limit.  Reached depth SR_NESTED_FRAME_MAX_DEPTH
 * is the last one served; the next acquire fails closed.  No production path
 * nests deeper than 2 today (a GE finish callback that itself submits a list);
 * 4 leaves headroom without letting runaway re-entrancy consume the pool. */
#define SR_NESTED_FRAME_MAX_DEPTH 4u

/* An acquire that fails returns 0 and leaves *sp_out / *handle_out untouched.
 * Callers must treat that as "do not dispatch": there is no correct stack to
 * hand the callee, and the pre-module behaviour (dispatch onto a frame someone
 * else owns) is the defect this module exists to remove. */
int sr_nested_frame_acquire(uint32_t owner, uint32_t *sp_out, int *handle_out);

/* Returns 1 when both guard bands were intact, 0 when corruption was detected.
 * The slot is returned to the pool either way -- refusing to reclaim it would
 * turn one overflow into permanent exhaustion.  A corrupt slot is reported once
 * per occurrence and counted. */
int sr_nested_frame_release(int handle);

/* Nonlocal-exit recovery.  Returns how many frames were reclaimed, so a caller
 * on a path where the count is expected to be zero can say so out loud. */
unsigned sr_nested_frame_release_owner(uint32_t owner);
unsigned sr_nested_frame_release_all(void);

/* Drop every frame and every counter.  For fixtures; production never calls it. */
void sr_nested_frame_reset(void);

/* ---- observation ---- */
void     sr_nested_frame_region(uint32_t *base, uint32_t *end);
unsigned sr_nested_frame_live(void);
unsigned sr_nested_frame_owner_depth(uint32_t owner);
unsigned sr_nested_frame_guard_failures(void);
unsigned sr_nested_frame_exhaustions(void);
unsigned sr_nested_frame_lifo_violations(void);
/* Describe a live handle.  Returns 0 for an unknown or free handle. */
int sr_nested_frame_handle_info(int handle, uint32_t *owner, unsigned *depth,
                                uint32_t *base, uint32_t *sp);
/* The usable stack extent of a live handle: [floor, sp). */
int sr_nested_frame_handle_extent(int handle, uint32_t *floor, uint32_t *sp);

#endif /* SR_NESTED_FRAMES_H */
