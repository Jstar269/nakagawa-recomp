// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
// Tiny assembly trampoline for initial register snapshot.
// Captures a0-a3, t0-t3, gp, sp, ra at the first instruction of the
// child thread before any C prologue modifies them. The snapshot must
// not modify the values before capture. Unavoidable effect: the store
// itself uses the stack and a temporary register; sp is captured before
// the frame is allocated, gp is the kernel-set module GP at entry.
// PSPDEV_BUILD = NOT RUN without successful compilation; REGISTER_CAPTURE_BUILD_VALIDATION = REQUIRED_BEFORE_HARDWARE.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <psptypes.h>

typedef struct {
    uint32_t a0, a1, a2, a3;
    uint32_t t0, t1, t2, t3;
    uint32_t gp, sp, ra;
    uint32_t hi, lo;
    uint32_t canary;
} ThreadEntrySnapshot;

// Compile-time offset assertions so snapshot layout cannot silently drift.
_Static_assert(offsetof(ThreadEntrySnapshot, a0) == 0, "snapshot offset a0 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, a1) == 4, "snapshot offset a1 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, a2) == 8, "snapshot offset a2 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, a3) == 12, "snapshot offset a3 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, t0) == 16, "snapshot offset t0 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, t1) == 20, "snapshot offset t1 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, t2) == 24, "snapshot offset t2 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, t3) == 28, "snapshot offset t3 mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, gp) == 32, "snapshot offset gp mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, sp) == 36, "snapshot offset sp mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, ra) == 40, "snapshot offset ra mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, hi) == 44, "snapshot offset hi mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, lo) == 48, "snapshot offset lo mismatch");
_Static_assert(offsetof(ThreadEntrySnapshot, canary) == 52, "snapshot offset canary mismatch");
_Static_assert(sizeof(ThreadEntrySnapshot) == 56, "snapshot size mismatch");

extern volatile ThreadEntrySnapshot g_thread_snapshot;
extern volatile int g_snapshot_valid;

// Assembly entry; jumps to thread_entry_c after capture. Declared with the
// firmware thread-entry prototype so CreateThread call sites type-check;
// the symbol itself is raw assembly (no C prologue) and never returns.
int thread_entry_shim(SceSize args, void *argp);

// C handler called after snapshot; receives original a0/a1.
int thread_entry_c(int argSize, void *argp);
