// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <stdint.h>

#define DMAC_INVALID_PRE_GUARD_BYTES 0x1000u
#define DMAC_INVALID_PAYLOAD_BYTES 0x0000c000u
#define DMAC_INVALID_POST_GUARD_BYTES 0x1000u
#define DMAC_INVALID_OVERFLOW_BAND_BYTES 0x2000u
#define DMAC_INVALID_TAIL_GUARD_BYTES 0x1000u
#define DMAC_INVALID_MAX_DELTA 0x2000u
#define DMAC_INVALID_PRE_GUARD_FILL 0x5au
#define DMAC_INVALID_PAYLOAD_FILL 0xc3u
#define DMAC_INVALID_POST_GUARD_FILL 0xc3u
#define DMAC_INVALID_OVERFLOW_BAND_FILL 0xa5u
#define DMAC_INVALID_TAIL_GUARD_FILL 0x96u
#define DMAC_INVALID_MAX_REQUEST \
    (DMAC_INVALID_PAYLOAD_BYTES + DMAC_INVALID_MAX_DELTA)
#define DMAC_INVALID_SCRATCH_BYTES \
    (DMAC_INVALID_PRE_GUARD_BYTES + DMAC_INVALID_PAYLOAD_BYTES + \
     DMAC_INVALID_POST_GUARD_BYTES + DMAC_INVALID_OVERFLOW_BAND_BYTES + \
     DMAC_INVALID_TAIL_GUARD_BYTES)

_Static_assert(
    DMAC_INVALID_PAYLOAD_BYTES + DMAC_INVALID_MAX_DELTA <=
        DMAC_INVALID_PAYLOAD_BYTES + DMAC_INVALID_POST_GUARD_BYTES +
            DMAC_INVALID_OVERFLOW_BAND_BYTES,
    "DMAC invalid-tail request must remain inside its owned scratch block");

#if defined(__mips__)
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspdmac.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <psputils.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <pspaudio.h>
#include <pspgu.h>
#include <pspgum.h>
#if PSP_ORACLE_CASE == 66
#include <pspreg.h>
#endif
#if PSP_ORACLE_CASE == 67
/* User-mode sceDisplay and sceImpose functions only: a user-mode PRX that
   imports the kernel libraries sceDisplay_driver or sceImpose_driver fails to
   load on the PSP. display_user_imports.S supplies the sceDisplay stubs. The
   PSPSDK libpspuser.a supplies the user sceImpose stubs; it names NID
   0x8C943191 (sceImposeGetBatteryIconStatus) sceImposeBatteryIconStatus. The
   SDK's pspimpose_driver.h is deliberately not included: it describes the
   kernel-only sceImpose_driver library. */
int sceDisplaySetHoldMode(int mode);
int sceDisplayWaitVblankStartMultiCB(unsigned int count);
int sceImposeBatteryIconStatus(int *charging, int *icon_status);
int sceImposeGetUMDPopup(void);
int sceImposeSetUMDPopup(int value);
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* source_commit is device-reported build identity, not a host-side rewrite.
   The Makefile supplies the full object id only from a clean checkout. */
#ifndef PROBE_BUILD_COMMIT
#error "PROBE_BUILD_COMMIT must be the full git object id this probe was built from"
#endif

PSP_MODULE_INFO("NAKAGAWA_PSP_ORACLE", 0, 1, 0);

#define FIXTURE_BUILD_ID "nakagawa-psp-oracle-v1"

#ifndef PSP_ORACLE_CASE
#define PSP_ORACLE_CASE 0
#endif

#define PSP_ORACLE_CASE_SMOKE 0
#define PSP_ORACLE_CASE_CALLBACK 1
#define PSP_ORACLE_CASE_WAIT_CANCEL 2
#define PSP_ORACLE_CASE_THREAD_LIFECYCLE 3
#define PSP_ORACLE_CASE_THREAD_DELETE 4
#define PSP_ORACLE_CASE_THREAD_DELETE_FOLLOWUP 5
#define PSP_ORACLE_CASE_THREAD_DELETE_EXPLICIT 6
#define PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY 7
#define PSP_ORACLE_CASE_DMAC_CONCURRENCY 8
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST 9
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_SRC 10
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_DST 11
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC 12
#define PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT 13
#define PSP_ORACLE_CASE_DISPLAY_MASK_DUTY 14
#define PSP_ORACLE_CASE_DISPLAY_GE_MASK 15
#define PSP_ORACLE_CASE_TRANSPORT_WRITE 44
#define PSP_ORACLE_CASE_THREAD_EXIT_DELETE 45
#define PSP_ORACLE_CASE_DMAC_SURVEY 46
#define PSP_ORACLE_CASE_CTRL_CLOCK 47
#define PSP_ORACLE_CASE_FPU_VECTOR 48
#define PSP_ORACLE_CASE_TEARDOWN_TEST 49
#define PSP_ORACLE_CASE_IO_MATRIX 50
#define PSP_ORACLE_CASE_AUDIO_QUERY 51
#define PSP_ORACLE_CASE_CACHE_ALIAS 52
#define PSP_ORACLE_CASE_DMAC_SIZE_MATRIX 53
#define PSP_ORACLE_CASE_MODEL_PROFILE 54
#define PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL 55
#define PSP_ORACLE_CASE_MBX_DELETE_WAIT 56
#define PSP_ORACLE_CASE_GE_NAN 57
#define PSP_ORACLE_CASE_DMAC_CELLS 58
#define PSP_ORACLE_CASE_DELAY_ZERO 59
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0 60
#define PSP_ORACLE_CASE_KERNEL_ALARM 61
#define PSP_ORACLE_CASE_THREAD_SCHEDULER 62
#define PSP_ORACLE_CASE_WAIT_OUTCOMES 63
#define PSP_ORACLE_CASE_GE_BREAK_CONTINUE 64
#define PSP_ORACLE_CASE_REFER_STATUS_SIZE 65
#define PSP_ORACLE_CASE_REGISTRY_READONLY 66
#define PSP_ORACLE_CASE_KERNEL_MISC 67
#define PSP_ORACLE_CASE_VFPU_COMPARE 68

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MODEL_PROFILE
#include <kubridge.h>
#endif
#define PSP_ORACLE_CASE_DISPLAY_WAIT_LATE 16
#define PSP_ORACLE_CASE_DISPLAY_WAIT_PRIORITY 17
#define PSP_ORACLE_CASE_DISPLAY_VBLANK_WINDOW 18
#define PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED 19
#define PSP_ORACLE_CASE_MUTEX_TIMEOUT_QUANTA 20
#define PSP_ORACLE_CASE_MUTEX_PRIORITY_INHERITANCE 21
#define PSP_ORACLE_CASE_MUTEX_INTERRUPT_CONTEXT 22

/* Plain mutex syscalls are absent from the installed PSPSDK headers, so the
   probe declares the exact ABI it imports via fixtures/psp_oracle/
   mutex_imports.S (ThreadManForUser NIDs). The struct mirrors the documented
   SceKernelMutexStatus layout; only scalar fields are treated as evidence. */
#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED
typedef struct SceKernelMutexInfo {
    SceSize size;
    char name[32];
    SceUInt attr;
    int initCount;
    int currentCount;
    SceUID lockThread;
    int numWaitThreads;
} SceKernelMutexInfo;

SceUID sceKernelCreateMutex(const char *name, SceUInt attr, int initCount, void *options);
int sceKernelDeleteMutex(SceUID mutexid);
int sceKernelLockMutex(SceUID mutexid, int count, uint32_t *pTimeout);
int sceKernelLockMutexCB(SceUID mutexid, int count, uint32_t *pTimeout);
int sceKernelTryLockMutex(SceUID mutexid, int count);
int sceKernelUnlockMutex(SceUID mutexid, int count);
int sceKernelCancelMutex(SceUID mutexid, int count, int *pNumWaitThreads);
int sceKernelReferMutexStatus(SceUID mutexid, SceKernelMutexInfo *info);
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
PSP_MAIN_THREAD_PARAMS(0x20, 32, THREAD_ATTR_USER);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN || \
      PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE
/* vadd.s/vmul.s run on the main thread; without the VFPU attribute the first
 * VFPU instruction traps (measured on PSP-3001 6.6.1: the thread stops after META).
 * The VFPU compare case runs vscmp.s/vsge.s/vslt.s there for the same reason. */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#else
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0
/* The default newlib heap claims the largest free partition block. A bounded
   heap leaves the high-address system-memory allocation available to the
   invalid-tail probe without relying on unowned memory. */
PSP_HEAP_SIZE_KB(512);
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
/* Same bounded heap as the tails: the default heap demonstrably squats the
   surveyed range (32/32 clean-fails), while the bounded one leaves the free
   pool observable. Survey and tails must share heap geometry or their
   results are incomparable. */
PSP_HEAP_SIZE_KB(512);
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL
/* Leave a bounded main-thread heap so the matrix can reserve both DMA spans
   from partition 2 instead of borrowing unowned VRAM. */
PSP_HEAP_SIZE_KB(512);
#endif

/* PPSSPP exposes a pseudo-device that headless builds use to capture test
   output; see Core/HLE/sceIo.cpp. Real hardware has no such device and the
   devctl simply fails, which is how the probe tells the two apart. The same
   record text is emitted either way, but the `source=` field differs so an
   emulator capture can never be compared as if it were hardware. */
#define EMULATOR_DEVCTL_SEND_OUTPUT 2
#define EMULATOR_DEVCTL_IS_EMULATOR 3

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_SMOKE
/* Keep this arithmetic body as a distinct guest function.  The Nakagawa smoke
   route recompiles this same PSP ELF and executes the generated function; an
   inlined host-side copy would not be an oracle producer. */
__attribute__((noinline)) uint32_t nakagawa_psp_oracle_sum_u32(uint32_t count) {
    uint32_t sum = 0;
    for (uint32_t value = 1; value <= count; ++value) {
        sum += value;
    }
    return sum;
}
#endif

static int emulator_present(void) {
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", EMULATOR_DEVCTL_IS_EMULATOR, NULL, 0, &flag, sizeof(flag)) < 0) {
        return 0;
    }
    return flag == 1;
}

static void emit(int emulated, const char *text) {
    if (emulated) {
        sceIoDevctl("emulator:", EMULATOR_DEVCTL_SEND_OUTPUT, (void *)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
        fflush(stdout);
    }
}

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
#define PROBE_HOST0_LOG "host0:/fpu_vector_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
#define PROBE_HOST0_LOG "host0:/io_matrix_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
#define PROBE_HOST0_LOG "host0:/audio_query_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
#define PROBE_HOST0_LOG "host0:/cache_alias_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL
#define PROBE_HOST0_LOG "host0:/dmac_size_matrix_cell_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0
#define PROBE_HOST0_LOG "host0:/dmac_invalid_tail_s0_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX
#define PROBE_HOST0_LOG "host0:/dmac_size_matrix_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MODEL_PROFILE
#define PROBE_HOST0_LOG "host0:/model_profile_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
#define PROBE_HOST0_LOG "host0:/dmac_survey_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST
#define PROBE_HOST0_LOG "host0:/dmac_invalid_tail_memcpy_dst_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_SRC
#define PROBE_HOST0_LOG "host0:/dmac_invalid_tail_memcpy_src_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_DST
#define PROBE_HOST0_LOG "host0:/dmac_invalid_tail_try_dst_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
#define PROBE_HOST0_LOG "host0:/dmac_invalid_tail_try_src_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_TRANSPORT_WRITE
#define PROBE_HOST0_LOG "host0:/transport_write_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MBX_DELETE_WAIT
#define PROBE_HOST0_LOG "host0:/mbx_delete_wait_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN
#define PROBE_HOST0_LOG "host0:/ge_nan_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS
#define PROBE_HOST0_LOG "host0:/dmac_cells_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO
#define PROBE_HOST0_LOG "host0:/delay_zero_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_KERNEL_ALARM
#define PROBE_HOST0_LOG "host0:/kernel_alarm_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_SCHEDULER
#define PROBE_HOST0_LOG "host0:/thread_scheduler_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_OUTCOMES
#define PROBE_HOST0_LOG "host0:/wait_outcomes_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_BREAK_CONTINUE
#define PROBE_HOST0_LOG "host0:/ge_break_continue_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_REFER_STATUS_SIZE
#define PROBE_HOST0_LOG "host0:/refer_status_size_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_READONLY
#define PROBE_HOST0_LOG "host0:/registry_readonly_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_KERNEL_MISC
#define PROBE_HOST0_LOG "host0:/kernel_misc_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE
#define PROBE_HOST0_LOG "host0:/vfpu_compare_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_SMOKE
#define PROBE_HOST0_LOG "host0:/smoke_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_EXIT_DELETE
#define PROBE_HOST0_LOG "host0:/thread_exit_delete_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_TEARDOWN_TEST
#define PROBE_HOST0_LOG "host0:/teardown_test_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY
#define PROBE_HOST0_LOG "host0:/display_mask_duty_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_LATE
#define PROBE_HOST0_LOG "host0:/display_wait_late_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_PRIORITY
#define PROBE_HOST0_LOG "host0:/display_wait_priority_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_VBLANK_WINDOW
#define PROBE_HOST0_LOG "host0:/display_vblank_window_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED
#define PROBE_HOST0_LOG "host0:/mutex_refer_unlocked_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_TIMEOUT_QUANTA
#define PROBE_HOST0_LOG "host0:/mutex_timeout_quanta_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_PRIORITY_INHERITANCE
#define PROBE_HOST0_LOG "host0:/mutex_priority_inheritance_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_INTERRUPT_CONTEXT
#define PROBE_HOST0_LOG "host0:/mutex_interrupt_context_log.txt"
#endif

/* Durable line writer: every record, step marker and the metadata line goes
   to the probe output (PSPLink stdout or the emulator channel) and, on
   hardware, is appended to the case's host0 log, which is closed again
   before the writer returns.  Closing after every line is what makes the log
   durable: a launch that later hangs or faults keeps every line written so
   far.  The campaign runner reads the per-case host0 log; stdout stays a
   secondary copy. */
static void probe_emit_durable(int emulated, const char *line, size_t length) {
    emit(emulated, line);
#ifdef PROBE_HOST0_LOG
    if (!emulated) {
        SceUID fd = sceIoOpen(PROBE_HOST0_LOG,
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            size_t offset = 0;
            while (offset < length) {
                const int wrote = sceIoWrite(fd, line + offset,
                                             (SceSize)(length - offset));
                if (wrote <= 0) break;
                offset += (size_t)wrote;
            }
            (void)sceIoClose(fd);
        }
    }
#else
    (void)length;
#endif
}

/* Durable progress marker.  Before a call that could hang or fault the
   console, a probe writes
       NAKAGAWA_PSP_STEP schema=1 case_id=<id> step=<name>
   through probe_emit_durable(), the same writer every record uses, so it is
   in the host0 log (closed) before the call runs.  A launch that never returns then still names its last step in the
   host log.  Step lines are progress, not results: the host parser
   (tools/psp_oracle/protocol.py) collects them and never counts them as
   records.  Names use [A-Za-z0-9_.:/-] only. */
__attribute__((unused))
static void probe_step(int emulated, const char *case_id, const char *step) {
    char line[224];
    const int used = snprintf(line, sizeof(line),
                              "NAKAGAWA_PSP_STEP schema=1 case_id=%s step=%s\n",
                              case_id, step);
    if (used <= 0 || (size_t)used >= sizeof(line)) return;
    probe_emit_durable(emulated, line, (size_t)used);
}

#define PROBE_TEARDOWN_CAPACITY 64u
#define PROBE_TEARDOWN_PATH_CAPACITY 16u
#define PROBE_TEARDOWN_PATH_LENGTH 96u

enum {
    PROBE_TEARDOWN_FAILED = -1,
    PROBE_TEARDOWN_SKIPPED = 0,
    PROBE_TEARDOWN_PASSED = 1,
};
#define PROBE_HOST0_ROUNDTRIP_PATH "host0:/nakagawa_transport_write.bin"

typedef int (*ProbeUidDeleteFn)(SceUID);

struct probe_owned_uid {
    SceUID uid;
    int active;
};

struct probe_owned_object {
    SceUID uid;
    ProbeUidDeleteFn delete_fn;
    int active;
};

struct probe_owned_audio {
    int channel;
    int (*release_fn)(int);
    int active;
};

struct probe_owned_subintr {
    int intr;
    int sub;
    int (*release_fn)(int, int);
    int active;
};

struct probe_owned_path {
    char path[PROBE_TEARDOWN_PATH_LENGTH];
    int active;
};

static struct probe_owned_uid s_owned_threads[PROBE_TEARDOWN_CAPACITY];
static struct probe_owned_object s_owned_objects[PROBE_TEARDOWN_CAPACITY];
static struct probe_owned_uid s_owned_memory[PROBE_TEARDOWN_CAPACITY];
static struct probe_owned_uid s_owned_fds[PROBE_TEARDOWN_CAPACITY];
static struct probe_owned_audio s_owned_audio[PROBE_TEARDOWN_CAPACITY];
static struct probe_owned_subintr s_owned_subintr[PROBE_TEARDOWN_CAPACITY];
static struct probe_owned_path s_owned_paths[PROBE_TEARDOWN_PATH_CAPACITY];
static int s_teardown_tracking_failed;
static int s_boot_cpu_mhz;
static int s_boot_bus_mhz;
static int s_have_clock_state;
static int s_pending_intr_tokens[PROBE_TEARDOWN_CAPACITY];
static int s_pending_intr_active[PROBE_TEARDOWN_CAPACITY];
#if defined(__mips__)
static uint32_t s_boot_fcr31;
static int s_have_fcr31_state;
#endif

static int probe_track_uid(struct probe_owned_uid *entries, SceUID uid) {
    if (uid < 0) return uid;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!entries[i].active) {
            entries[i].uid = uid;
            entries[i].active = 1;
            return uid;
        }
    }
    s_teardown_tracking_failed = 1;
    return uid;
}

static void probe_untrack_uid(struct probe_owned_uid *entries, SceUID uid) {
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (entries[i].active && entries[i].uid == uid) {
            entries[i].active = 0;
        }
    }
}

SceUID probe_track_thread(SceUID uid) {
    return probe_track_uid(s_owned_threads, uid);
}

int probe_delete_thread(SceUID uid) {
    const int result = sceKernelDeleteThread(uid);
    if (result >= 0) probe_untrack_uid(s_owned_threads, uid);
    return result;
}

int probe_terminate_delete_thread(SceUID uid) {
    const int result = sceKernelTerminateDeleteThread(uid);
    if (result >= 0) probe_untrack_uid(s_owned_threads, uid);
    return result;
}

SceUID probe_track_kernel_object(SceUID uid, ProbeUidDeleteFn delete_fn) {
    if (uid < 0) return uid;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_objects[i].active) {
            s_owned_objects[i].uid = uid;
            s_owned_objects[i].delete_fn = delete_fn;
            s_owned_objects[i].active = 1;
            return uid;
        }
    }
    s_teardown_tracking_failed = 1;
    return uid;
}

int probe_delete_kernel_object(SceUID uid, ProbeUidDeleteFn delete_fn) {
    const int result = delete_fn(uid);
    if (result >= 0) {
        for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
            if (s_owned_objects[i].active && s_owned_objects[i].uid == uid &&
                s_owned_objects[i].delete_fn == delete_fn) {
                s_owned_objects[i].active = 0;
            }
        }
    }
    return result;
}

SceUID probe_track_partition_memory(SceUID uid) {
    return probe_track_uid(s_owned_memory, uid);
}

int probe_free_partition_memory(SceUID uid) {
    const int result = sceKernelFreePartitionMemory(uid);
    if (result >= 0) probe_untrack_uid(s_owned_memory, uid);
    return result;
}

static int probe_is_capture_path(const char *path) {
#ifdef PROBE_HOST0_LOG
    if (strcmp(path, PROBE_HOST0_LOG) == 0) return 1;
#endif
    return strcmp(path, PROBE_HOST0_ROUNDTRIP_PATH) == 0;
}

static void probe_track_disposable_path(const char *path) {
    if (strncmp(path, "host0:/", 7) != 0 || probe_is_capture_path(path)) return;
    for (size_t i = 0; i < PROBE_TEARDOWN_PATH_CAPACITY; i++) {
        if (s_owned_paths[i].active && strcmp(s_owned_paths[i].path, path) == 0) return;
        if (!s_owned_paths[i].active) {
            const size_t length = strlen(path);
            if (length >= sizeof(s_owned_paths[i].path)) {
                s_teardown_tracking_failed = 1;
                return;
            }
            memcpy(s_owned_paths[i].path, path, length + 1);
            s_owned_paths[i].active = 1;
            return;
        }
    }
    s_teardown_tracking_failed = 1;
}

SceUID probe_io_open(const char *path, int flags, SceMode mode) {
    const SceUID fd = sceIoOpen(path, flags, mode);
    if (fd >= 0) {
        (void)probe_track_uid(s_owned_fds, fd);
        if ((flags & PSP_O_CREAT) != 0) probe_track_disposable_path(path);
    }
    return fd;
}

int probe_io_close(SceUID fd) {
    const int result = sceIoClose(fd);
    if (result >= 0) probe_untrack_uid(s_owned_fds, fd);
    return result;
}

int probe_io_remove(const char *path) {
    const int result = sceIoRemove(path);
    if (result >= 0) {
        for (size_t i = 0; i < PROBE_TEARDOWN_PATH_CAPACITY; i++) {
            if (s_owned_paths[i].active && strcmp(s_owned_paths[i].path, path) == 0) {
                s_owned_paths[i].active = 0;
            }
        }
    }
    return result;
}

int probe_track_audio_channel(int channel, int (*release_fn)(int)) {
    if (channel < 0) return channel;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_audio[i].active) {
            s_owned_audio[i].channel = channel;
            s_owned_audio[i].release_fn = release_fn;
            s_owned_audio[i].active = 1;
            return channel;
        }
    }
    s_teardown_tracking_failed = 1;
    return channel;
}

static void probe_untrack_audio_channel(int channel) {
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (s_owned_audio[i].active && s_owned_audio[i].channel == channel) {
            s_owned_audio[i].active = 0;
        }
    }
}

int probe_release_audio_channel(int channel) {
    const int result = sceAudioChRelease(channel);
    if (result >= 0) probe_untrack_audio_channel(channel);
    return result;
}

void probe_track_subintr(int intr, int sub, int (*release_fn)(int, int)) {
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_subintr[i].active) {
            s_owned_subintr[i].intr = intr;
            s_owned_subintr[i].sub = sub;
            s_owned_subintr[i].release_fn = release_fn;
            s_owned_subintr[i].active = 1;
            return;
        }
    }
    s_teardown_tracking_failed = 1;
}

int probe_release_subintr(int intr, int sub) {
    const int result = sceKernelReleaseSubIntrHandler(intr, sub);
    if (result >= 0) {
        for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
            if (s_owned_subintr[i].active && s_owned_subintr[i].intr == intr &&
                s_owned_subintr[i].sub == sub) {
                s_owned_subintr[i].active = 0;
            }
        }
    }
    return result;
}

static void probe_track_intr_token(int token) {
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_pending_intr_active[i]) {
            s_pending_intr_tokens[i] = token;
            s_pending_intr_active[i] = 1;
            return;
        }
    }
    s_teardown_tracking_failed = 1;
}

static int probe_untrack_intr_token(int token) {
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (s_pending_intr_active[i] && s_pending_intr_tokens[i] == token) {
            s_pending_intr_active[i] = 0;
            return 1;
        }
    }
    return 0;
}

int probe_suspend_intr(void) {
    const int token = sceKernelCpuSuspendIntr();
    probe_track_intr_token(token);
    return token;
}

int probe_resume_intr(int token) {
    sceKernelCpuResumeIntr(token);
    /* IsCpuIntrSuspended interprets the saved token; IsCpuIntrEnable verifies
       the live post-resume state.  The resume contract checks the
       complementary CPU-enabled state: token 0 means interrupts remain
       suspended, while a nonzero token restores delivery. */
    const int expected_enabled = !sceKernelIsCpuIntrSuspended((unsigned int)token);
    if (sceKernelIsCpuIntrEnable() != expected_enabled) {
        s_teardown_tracking_failed = 1;
        return -1;
    }
    if (!probe_untrack_intr_token(token)) {
        s_teardown_tracking_failed = 1;
        return -1;
    }
    return 0;
}

#define sceKernelCreateThread(...) \
    probe_track_thread(sceKernelCreateThread(__VA_ARGS__))
#define sceKernelDeleteThread(uid) probe_delete_thread(uid)
#define sceKernelTerminateDeleteThread(uid) probe_terminate_delete_thread(uid)
#define sceKernelCreateCallback(...) \
    probe_track_kernel_object(sceKernelCreateCallback(__VA_ARGS__), sceKernelDeleteCallback)
#define sceKernelDeleteCallback(uid) \
    probe_delete_kernel_object((uid), sceKernelDeleteCallback)
#define sceKernelCreateSema(...) \
    probe_track_kernel_object(sceKernelCreateSema(__VA_ARGS__), sceKernelDeleteSema)
#define sceKernelDeleteSema(uid) probe_delete_kernel_object((uid), sceKernelDeleteSema)
#define sceKernelCreateEventFlag(...) \
    probe_track_kernel_object(sceKernelCreateEventFlag(__VA_ARGS__), sceKernelDeleteEventFlag)
#define sceKernelDeleteEventFlag(uid) \
    probe_delete_kernel_object((uid), sceKernelDeleteEventFlag)
#define sceKernelCreateMbx(...) \
    probe_track_kernel_object(sceKernelCreateMbx(__VA_ARGS__), sceKernelDeleteMbx)
#define sceKernelDeleteMbx(uid) probe_delete_kernel_object((uid), sceKernelDeleteMbx)
#define sceKernelCreateVTimer(...) \
    probe_track_kernel_object(sceKernelCreateVTimer(__VA_ARGS__), sceKernelDeleteVTimer)
#define sceKernelDeleteVTimer(uid) \
    probe_delete_kernel_object((uid), sceKernelDeleteVTimer)
#define sceKernelCreateFpl(...) \
    probe_track_kernel_object(sceKernelCreateFpl(__VA_ARGS__), sceKernelDeleteFpl)
#define sceKernelDeleteFpl(uid) probe_delete_kernel_object((uid), sceKernelDeleteFpl)
#define sceKernelCreateVpl(...) \
    probe_track_kernel_object(sceKernelCreateVpl(__VA_ARGS__), sceKernelDeleteVpl)
#define sceKernelDeleteVpl(uid) probe_delete_kernel_object((uid), sceKernelDeleteVpl)
#define sceKernelAllocPartitionMemory(...) \
    probe_track_partition_memory(sceKernelAllocPartitionMemory(__VA_ARGS__))
#define sceKernelFreePartitionMemory(uid) probe_free_partition_memory(uid)
#define sceIoOpen(path, flags, mode) probe_io_open((path), (flags), (mode))
#define sceIoClose(fd) probe_io_close(fd)
#define sceIoRemove(path) probe_io_remove(path)
#define sceAudioChReserve(...) \
    probe_track_audio_channel(sceAudioChReserve(__VA_ARGS__), sceAudioChRelease)
#define sceAudioChRelease(channel) probe_release_audio_channel(channel)
#define sceKernelReleaseSubIntrHandler(intr, sub) probe_release_subintr((intr), (sub))
#define sceKernelCpuSuspendIntr() probe_suspend_intr()
#define sceKernelCpuResumeIntr(token) probe_resume_intr(token)

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_MUTEX_INTERRUPT_CONTEXT
#define sceKernelCreateMutex(...) \
    probe_track_kernel_object(sceKernelCreateMutex(__VA_ARGS__), sceKernelDeleteMutex)
#define sceKernelDeleteMutex(uid) \
    probe_delete_kernel_object((uid), sceKernelDeleteMutex)
#endif

#if PSP_ORACLE_CASE != PSP_ORACLE_CASE_SMOKE
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CALLBACK || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_CANCEL || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_LIFECYCLE
static void emit_test(int emulated, const char *case_id, int pass,
                      uint32_t result, uint32_t out0, uint32_t out1,
                      uint32_t out2, uint32_t out3) {
    char line[320];
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-KERNEL-001 case_id=%s "
             "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x\n",
             case_id, pass ? "PASS" : "FAIL", (unsigned int)result,
             (unsigned int)out0, (unsigned int)out1,
             (unsigned int)out2, (unsigned int)out3);
    probe_emit_durable(emulated, line, strlen(line));
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_THREAD_DELETE
/* Some launches emit no extended records (the DMA invalid-tail launches do not),
   so the helper is unused in those builds; it is not an error there. */
__attribute__((unused))
static void emit_record_extended(int emulated, const char *test_id,
                                 const char *case_id, const char *status,
                                 uint32_t result, const uint32_t *out,
                                 size_t out_count) {
    char line[1024];
    int used = snprintf(line, sizeof(line),
                        "NAKAGAWA_PSP_TEST schema=1 test_id=%s "
                        "case_id=%s status=%s result=0x%08x",
                        test_id, case_id, status, (unsigned int)result);
    for (size_t i = 0; i < out_count && used > 0 && (size_t)used < sizeof(line); i++) {
        int wrote = snprintf(line + used, sizeof(line) - (size_t)used,
                             " out%u=0x%08x", (unsigned int)i,
                             (unsigned int)out[i]);
        if (wrote < 0) break;
        used += wrote;
    }
    if (used > 0 && (size_t)used + 1 < sizeof(line)) {
        line[used++] = '\n';
        line[used] = '\0';
    }
    probe_emit_durable(emulated, line, strlen(line));
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO
/* Deferred record buffer.
   The IO matrix measures IoFileMgr itself and the cache matrix measures
   dcache line residency across cells.  Emitting a record inside the measured
   window would perturb the very state under test: `emit_record_extended`
   opens, writes and closes a `host0:` descriptor, which allocates a fresh
   SceUID from the same IoFileMgr namespace the IO probe samples, and drives a
   blocking USB round trip whose kernel and DMA footprint can evict the
   `s_cache_buf` line whose residency the cache probe samples between cells.

   Records are therefore accumulated as raw values -- no formatting, no
   syscall, no allocation -- and emitted only after every measurement cell has
   run.  Capacity is a compile-time constant checked against the largest cell
   count either probe can emit, so overflow is impossible by construction
   rather than handled at runtime. */
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS
#define DEFERRED_MAX_RECORDS 128
#define DEFERRED_MAX_OUT 10
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN
#define DEFERRED_MAX_RECORDS 24
#define DEFERRED_MAX_OUT 6
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
#define DEFERRED_MAX_RECORDS 16
#define DEFERRED_MAX_OUT 8
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO
#define DEFERRED_MAX_RECORDS 4
#define DEFERRED_MAX_OUT 10
#else
#define DEFERRED_MAX_RECORDS 8
#define DEFERRED_MAX_OUT 6
#endif

struct deferred_record {
    char case_id[64]; /* copied so trial-indexed record names remain stable */
    const char *status;  /* "PASS" / "FAIL" literal */
    uint32_t result;
    uint32_t out[DEFERRED_MAX_OUT];
    size_t out_count;
};

static struct deferred_record s_deferred[DEFERRED_MAX_RECORDS];
static size_t s_deferred_count;

static void defer_record(const char *case_id, const char *status,
                         uint32_t result, const uint32_t *out,
                         size_t out_count) {
    struct deferred_record *rec = &s_deferred[s_deferred_count++];
    snprintf(rec->case_id, sizeof(rec->case_id), "%s", case_id);
    rec->status = status;
    rec->result = result;
    for (size_t i = 0; i < out_count; i++) {
        rec->out[i] = out[i];
    }
    rec->out_count = out_count;
}

/* Emit every buffered record in capture order.  Called only after the last
   measurement cell has completed, so no emission touches measured state. */
static void flush_deferred(int emulated, const char *test_id) {
    for (size_t i = 0; i < s_deferred_count; i++) {
        emit_record_extended(emulated, test_id, s_deferred[i].case_id,
                             s_deferred[i].status, s_deferred[i].result,
                             s_deferred[i].out, s_deferred[i].out_count);
    }
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_THREAD_DELETE && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY
static void emit_test_extended(int emulated, const char *case_id, int pass,
                               uint32_t result, const uint32_t *out,
                               size_t out_count) {
    emit_record_extended(emulated, "PSP-KERNEL-001", case_id,
                         pass ? "PASS" : "FAIL", result, out, out_count);
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_MUTEX_INTERRUPT_CONTEXT
static void emit_mutex_test(int emulated, const char *case_id, int pass,
                            uint32_t result, const uint32_t *out,
                            size_t out_count) {
    emit_record_extended(emulated, "PSP-MUTEX-001", case_id,
                         pass ? "PASS" : "FAIL", result, out, out_count);
}
#endif
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CALLBACK
static volatile int s_callback_calls;
static volatile int s_callback_arg1;
static volatile int s_callback_arg2;

static int oracle_callback(int arg1, int arg2, void *common) {
    (void)common;
    s_callback_calls++;
    s_callback_arg1 = arg1;
    s_callback_arg2 = arg2;
    return 0;
}

static int run_callback_case(uint32_t *out0, uint32_t *out1,
                             uint32_t *out2, uint32_t *out3) {
    s_callback_calls = 0;
    s_callback_arg1 = 0;
    s_callback_arg2 = 0;
    const SceUID cbid = sceKernelCreateCallback("oracle-callback", oracle_callback, NULL);
    if (cbid < 0) {
        *out0 = 0;
        *out1 = (uint32_t)cbid;
        return 0;
    }
    const int notify_first = sceKernelNotifyCallback(cbid, 0x1234);
    const int count_before = sceKernelGetCallbackCount(cbid);
    const int check = sceKernelCheckCallback();
    const int count_after = sceKernelGetCallbackCount(cbid);
    const int notify_second = sceKernelNotifyCallback(cbid, 0x5678);
    const int cancel = sceKernelCancelCallback(cbid);
    const int count_cancelled = sceKernelGetCallbackCount(cbid);
    const int delete_result = sceKernelDeleteCallback(cbid);

    /* Normalize control-flow predicates to bits so UIDs do not enter the comparison
       stream.  The raw cancellation/deletion returns remain in out2/out3 so an
       acceptance comparison also checks the PSP error-code contract. */
    *out0 = (uint32_t)(notify_first == 0) |
            ((uint32_t)(count_before == 1) << 1) |
            ((uint32_t)(check > 0) << 2) |
            ((uint32_t)(count_after == 0) << 3) |
            ((uint32_t)(notify_second == 0) << 4) |
            ((uint32_t)(cancel == 0) << 5) |
            ((uint32_t)(count_cancelled == 0) << 6) |
            ((uint32_t)(delete_result == 0) << 7) |
            ((uint32_t)(s_callback_calls == 1) << 8);
    *out0 |= ((uint32_t)(count_cancelled & 0xff) << 16);
    *out1 = ((uint32_t)(s_callback_arg1 & 0xffff) << 16) |
            (uint32_t)(s_callback_arg2 & 0xffff);
    *out2 = (uint32_t)cancel;
    *out3 = (uint32_t)delete_result;
    return notify_first == 0 && count_before == 1 && check > 0 && count_after == 0 &&
           notify_second == 0 && cancel == 0 && count_cancelled == 0 &&
           delete_result == 0 && s_callback_calls == 1;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_CANCEL
static uint32_t run_wait_cancel_case(uint32_t *out0, uint32_t *out1,
                                     uint32_t *out2, uint32_t *out3) {
    const SceUID semaid = sceKernelCreateSema("oracle-sema", 0, 0, 1, NULL);
    if (semaid < 0) {
        *out0 = 0;
        *out1 = (uint32_t)semaid;
        return 0;
    }
    const int empty = sceKernelPollSema(semaid, 1);
    const int signal = sceKernelSignalSema(semaid, 1);
    const int ready = sceKernelPollSema(semaid, 1);
    const int empty_again = sceKernelPollSema(semaid, 1);
    const int delete_result = sceKernelDeleteSema(semaid);
    *out0 = (uint32_t)(empty < 0) |
            ((uint32_t)(signal == 0) << 1) |
            ((uint32_t)(ready == 0) << 2) |
            ((uint32_t)(empty_again < 0) << 3) |
            ((uint32_t)(delete_result == 0) << 4);
    *out1 = 0;
    *out2 = (uint32_t)empty;
    *out3 = (uint32_t)empty_again;
    return *out0 == 0x1fu;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_LIFECYCLE
static int oracle_thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0x42;
}

static uint32_t run_thread_lifecycle_case(uint32_t *out0, uint32_t *out1,
                                          uint32_t *out2, uint32_t *out3) {
    const SceUID thid = sceKernelCreateThread("oracle-thread", oracle_thread_entry,
                                              0x20, 0x4000, 0, NULL);
    if (thid < 0) {
        *out0 = 0;
        *out1 = (uint32_t)thid;
        return 0;
    }
    const int start = sceKernelStartThread(thid, 0, NULL);
    const int wait = sceKernelWaitThreadEnd(thid, NULL);
    const int exit_status = sceKernelGetThreadExitStatus(thid);
    const int delete_result = sceKernelDeleteThread(thid);
    const int post_delete_status = sceKernelGetThreadExitStatus(thid);
    *out0 = (uint32_t)(start == 0) |
            ((uint32_t)(wait == 0x42) << 1) |
            ((uint32_t)(delete_result == 0) << 2) |
            ((uint32_t)(post_delete_status < 0) << 3);
    *out1 = ((uint32_t)exit_status & 0xffffu) |
            ((uint32_t)wait << 16);
    *out2 = (uint32_t)post_delete_status;
    *out3 = (uint32_t)delete_result;
    return start == 0 && wait >= 0 && delete_result == 0 && post_delete_status < 0 &&
           exit_status == 0x42;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE
static volatile SceUID s_join_target;
static volatile int s_join_result;

static int oracle_sleep_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelSleepThread();
    return 0x55;
}

static int oracle_exit_delete_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelExitDeleteThread(0x66);
    return -1;
}

static int oracle_join_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    s_join_result = sceKernelWaitThreadEnd(s_join_target, NULL);
    return s_join_result;
}

static uint32_t run_thread_delete_case(uint32_t *out0, uint32_t *out1,
                                       uint32_t *out2, uint32_t *out3,
                                       uint32_t *out4, uint32_t *out5,
                                       uint32_t *out6, uint32_t *out7,
                                       uint32_t *out8, uint32_t *out9) {
    const int invalid_delete = sceKernelDeleteThread(0x7fffffff);
    const int current_delete = sceKernelDeleteThread(sceKernelGetThreadId());

    const SceUID term_target = sceKernelCreateThread("oracle-term", oracle_sleep_entry,
                                                     0x30, 0x4000, 0, NULL);
    const SceUID term_joiner = sceKernelCreateThread("oracle-term-join", oracle_join_entry,
                                                     0x20, 0x4000, 0, NULL);
    s_join_target = term_target;
    s_join_result = 0;
    const int term_start = term_target < 0 ? term_target : sceKernelStartThread(term_target, 0, NULL);
    const int term_join_start = term_joiner < 0 ? term_joiner : sceKernelStartThread(term_joiner, 0, NULL);
    if (term_joiner >= 0) sceKernelDelayThread(1000);
    const int term_delete = term_target < 0 ? term_target : sceKernelTerminateDeleteThread(term_target);
    const int term_join_wait = term_joiner < 0 ? term_joiner : sceKernelWaitThreadEnd(term_joiner, NULL);
    const int term_join_result = s_join_result;
    const int term_post_status = term_target < 0 ? term_target : sceKernelGetThreadExitStatus(term_target);
    const int term_post_start = term_target < 0 ? term_target : sceKernelStartThread(term_target, 0, NULL);
    const int term_post_wake = term_target < 0 ? term_target : sceKernelWakeupThread(term_target);
    if (term_joiner >= 0) sceKernelDeleteThread(term_joiner);

    const SceUID exit_target = sceKernelCreateThread("oracle-exit-delete", oracle_exit_delete_entry,
                                                     0x30, 0x4000, 0, NULL);
    const SceUID exit_joiner = sceKernelCreateThread("oracle-exit-join", oracle_join_entry,
                                                     0x20, 0x4000, 0, NULL);
    s_join_target = exit_target;
    s_join_result = 0;
    const int exit_start = exit_target < 0 ? exit_target : sceKernelStartThread(exit_target, 0, NULL);
    const int exit_join_start = exit_joiner < 0 ? exit_joiner : sceKernelStartThread(exit_joiner, 0, NULL);
    if (exit_joiner >= 0) sceKernelDelayThread(1000);
    const int exit_join_wait = exit_joiner < 0 ? exit_joiner : sceKernelWaitThreadEnd(exit_joiner, NULL);
    const int exit_join_result = s_join_result;
    const int exit_post_status = exit_target < 0 ? exit_target : sceKernelGetThreadExitStatus(exit_target);
    if (exit_joiner >= 0) sceKernelDeleteThread(exit_joiner);

    *out0 = (uint32_t)(invalid_delete < 0) |
            ((uint32_t)((uint32_t)current_delete == 0x800201a4u) << 1) |
            ((uint32_t)(term_target >= 0 && term_start == 0 && term_join_start == 0) << 2) |
            ((uint32_t)(term_delete == 0) << 3) |
            ((uint32_t)((uint32_t)term_join_result == 0x800201acu) << 4) |
            ((uint32_t)((uint32_t)term_join_wait == 0x800201acu) << 5) |
            ((uint32_t)((uint32_t)term_post_status == 0x80020198u) << 6) |
            ((uint32_t)((uint32_t)term_post_start == 0x80020198u) << 7) |
            ((uint32_t)((uint32_t)term_post_wake == 0x80020198u) << 8) |
            ((uint32_t)(exit_target >= 0 && exit_start == 0 && exit_join_start == 0) << 9) |
            ((uint32_t)(exit_join_result == 0x66) << 10) |
            ((uint32_t)(exit_join_wait == 0x66) << 11) |
            ((uint32_t)((uint32_t)exit_post_status == 0x80020198u) << 12);
    *out1 = (uint32_t)invalid_delete;
    *out2 = (uint32_t)current_delete;
    *out3 = (uint32_t)term_delete;
    *out4 = (uint32_t)term_join_result;
    *out5 = (uint32_t)term_post_status;
    *out6 = (uint32_t)exit_join_result;
    *out7 = (uint32_t)exit_post_status;
    *out8 = (uint32_t)exit_join_wait;
    /* Diagnostic: bit 5 of out0 compares term_join_wait against
       SCE_KERNEL_ERROR_THREAD_TERMINATED, but the raw value was never
       emitted, so a hardware DIFFERENCE said only "not 0x800201ac".
       Emit it so the PSP's actual second-order join result is readable. */
    *out9 = (uint32_t)term_join_wait;
    return *out0 == 0x1fffu;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_FOLLOWUP || PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_EXPLICIT || PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY
/* This is the smallest control that separates the two live explanations for
   the second-order wait discrepancy.  Both joiners wait on a target that is
   terminate-deleted and both receive THREAD_TERMINATED from the inner wait.
   The semaphores prove that the inner wait was entered before deletion and
   returned before the outer wait and ReferThreadStatus are sampled.  The
   follow-up case uses implicit entry returns; the explicit sibling calls
   sceKernelExitThread with the same two status shapes. */
static volatile SceUID s_followup_join_target;
static volatile int s_followup_join_result;
static volatile int s_followup_join_exit_mode;
static volatile SceUID s_followup_waiting_sema;
static volatile SceUID s_followup_done_sema;

static int followup_sleep_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelSleepThread();
    return 0x55;
}

static int followup_join_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (s_followup_waiting_sema >= 0) {
        sceKernelSignalSema(s_followup_waiting_sema, 1);
    }
    s_followup_join_result = sceKernelWaitThreadEnd(s_followup_join_target, NULL);
    if (s_followup_done_sema >= 0) {
        sceKernelSignalSema(s_followup_done_sema, 1);
    }
    const int exit_mode = s_followup_join_exit_mode;
    if (exit_mode == 2) {
        (void)sceKernelExitThread((int)0x800201acu);
        return 0;
    }
    if (exit_mode == 3) {
        (void)sceKernelExitThread(0x78);
        return 0;
    }
    if (exit_mode == 4) {
        (void)sceKernelExitThread((int)0x800201a8u);
        return 0;
    }
    if (exit_mode == 5) {
        (void)sceKernelExitThread(-17);
        return 0;
    }
    return exit_mode == 1 ? 0x77 : s_followup_join_result;
}

static int followup_refer_status(SceUID thid, uint32_t *ret,
                                 uint32_t *status, uint32_t *wait_type,
                                 uint32_t *wait_id, uint32_t *exit_status) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    const int result = sceKernelReferThreadStatus(thid, &info);
    *ret = (uint32_t)result;
    *status = (uint32_t)info.status;
    *wait_type = (uint32_t)info.waitType;
    *wait_id = (uint32_t)info.waitId;
    *exit_status = (uint32_t)info.exitStatus;
    return result;
}

static uint32_t run_thread_delete_followup_case(uint32_t *out0, uint32_t *out1,
                                                uint32_t *out2, uint32_t *out3,
                                                uint32_t *out4, uint32_t *out5,
                                                uint32_t *out6, uint32_t *out7,
                                                uint32_t *out8, uint32_t *out9,
                                                uint32_t *out10, uint32_t *out11,
                                                uint32_t *out12, uint32_t *out13,
                                                uint32_t *out14,
                                                uint32_t *out15, uint32_t *out16,
                                                int exit_variant) {
    const int boundary = exit_variant == 2;
    const int explicit_exit = exit_variant != 0;
    const uint32_t expected_error_exit = 0x800200d2u;
    const uint32_t explicit_error = boundary ? 0x800201a8u : 0x800201acu;
    const uint32_t positive_exit_argument = boundary ? 0xfffffffefu :
                                                  (explicit_exit ? 0x78u : 0x77u);
    const uint32_t expected_positive_exit = boundary ? expected_error_exit :
                                                   positive_exit_argument;
    const int error_exit_mode = boundary ? 4 : (explicit_exit ? 2 : 0);
    const int positive_exit_mode = boundary ? 5 : (explicit_exit ? 3 : 1);
    const SceUID error_target = sceKernelCreateThread("oracle-follow-error-target",
                                                       followup_sleep_entry,
                                                       0x30, 0x4000, 0, NULL);
    const SceUID error_joiner = sceKernelCreateThread("oracle-follow-error-joiner",
                                                       followup_join_entry,
                                                       0x10, 0x4000, 0, NULL);
    const SceUID error_waiting_sema = sceKernelCreateSema("oracle-follow-error-waiting",
                                                          0, 0, 1, NULL);
    const SceUID error_done_sema = sceKernelCreateSema("oracle-follow-error-done",
                                                       0, 0, 1, NULL);
    s_followup_join_target = error_target;
    s_followup_join_result = 0;
    s_followup_join_exit_mode = error_exit_mode;
    s_followup_waiting_sema = error_waiting_sema;
    s_followup_done_sema = error_done_sema;
    const int error_sema_ok = error_waiting_sema >= 0 && error_done_sema >= 0;
    const int error_start = !error_sema_ok || error_target < 0
        ? -1 : sceKernelStartThread(error_target, 0, NULL);
    const int error_join_start = !error_sema_ok || error_joiner < 0
        ? -1 : sceKernelStartThread(error_joiner, 0, NULL);
    const int error_waiting = error_join_start < 0
        ? error_join_start : sceKernelWaitSema(error_waiting_sema, 1, NULL);
    const int error_delete = error_waiting < 0
        ? error_waiting : sceKernelTerminateDeleteThread(error_target);
    const int error_done = error_delete < 0 || error_joiner < 0
        ? error_delete : sceKernelWaitSema(error_done_sema, 1, NULL);
    const int error_inner = s_followup_join_result;

    uint32_t error_ref = 0;
    uint32_t error_status = 0;
    uint32_t error_wait_type = 0;
    uint32_t error_wait_id = 0;
    uint32_t error_exit_status = 0;
    const int error_outer = error_done < 0
        ? error_done
        : (error_joiner < 0 ? error_joiner : sceKernelWaitThreadEnd(error_joiner, NULL));
    const int error_ref_result = error_joiner < 0
        ? error_joiner
        : followup_refer_status(error_joiner, &error_ref, &error_status,
                                &error_wait_type, &error_wait_id, &error_exit_status);
    if (error_joiner >= 0) sceKernelDeleteThread(error_joiner);
    if (error_waiting_sema >= 0) sceKernelDeleteSema(error_waiting_sema);
    if (error_done_sema >= 0) sceKernelDeleteSema(error_done_sema);
    s_followup_waiting_sema = -1;
    s_followup_done_sema = -1;

    const SceUID positive_target = sceKernelCreateThread("oracle-follow-positive-target",
                                                          followup_sleep_entry,
                                                          0x30, 0x4000, 0, NULL);
    const SceUID positive_joiner = sceKernelCreateThread("oracle-follow-positive-joiner",
                                                          followup_join_entry,
                                                          0x10, 0x4000, 0, NULL);
    const SceUID positive_waiting_sema = sceKernelCreateSema("oracle-follow-positive-waiting",
                                                              0, 0, 1, NULL);
    const SceUID positive_done_sema = sceKernelCreateSema("oracle-follow-positive-done",
                                                           0, 0, 1, NULL);
    s_followup_join_target = positive_target;
    s_followup_join_result = 0;
    s_followup_join_exit_mode = positive_exit_mode;
    s_followup_waiting_sema = positive_waiting_sema;
    s_followup_done_sema = positive_done_sema;
    const int positive_sema_ok = positive_waiting_sema >= 0 && positive_done_sema >= 0;
    const int positive_start = !positive_sema_ok || positive_target < 0
        ? -1 : sceKernelStartThread(positive_target, 0, NULL);
    const int positive_join_start = !positive_sema_ok || positive_joiner < 0
        ? -1 : sceKernelStartThread(positive_joiner, 0, NULL);
    const int positive_waiting = positive_join_start < 0
        ? positive_join_start : sceKernelWaitSema(positive_waiting_sema, 1, NULL);
    const int positive_delete = positive_waiting < 0
        ? positive_waiting : sceKernelTerminateDeleteThread(positive_target);
    const int positive_done = positive_delete < 0 || positive_joiner < 0
        ? positive_delete : sceKernelWaitSema(positive_done_sema, 1, NULL);
    const int positive_inner = s_followup_join_result;

    uint32_t positive_ref = 0;
    uint32_t positive_status = 0;
    uint32_t positive_wait_type = 0;
    uint32_t positive_wait_id = 0;
    uint32_t positive_exit_status = 0;
    const int positive_outer = positive_done < 0
        ? positive_done
        : (positive_joiner < 0 ? positive_joiner : sceKernelWaitThreadEnd(positive_joiner, NULL));
    const int positive_ref_result = positive_joiner < 0
        ? positive_joiner
        : followup_refer_status(positive_joiner, &positive_ref, &positive_status,
                                &positive_wait_type, &positive_wait_id, &positive_exit_status);
    if (positive_joiner >= 0) sceKernelDeleteThread(positive_joiner);
    if (positive_waiting_sema >= 0) sceKernelDeleteSema(positive_waiting_sema);
    if (positive_done_sema >= 0) sceKernelDeleteSema(positive_done_sema);
    s_followup_waiting_sema = -1;
    s_followup_done_sema = -1;

    /* The mask records setup, semaphore handshakes, inner results, the
       measured negative-return normalization, and status-query observations.
       The raw fields remain in the record so a new firmware/model can expose
       a divergence without losing the observed scalars. */
    *out0 = (uint32_t)(error_target >= 0 && error_start == 0) |
            ((uint32_t)(error_joiner >= 0 && error_join_start == 0) << 1) |
            ((uint32_t)(error_delete == 0) << 2) |
            ((uint32_t)((uint32_t)error_inner == 0x800201acu) << 3) |
            ((uint32_t)(error_ref_result == 0) << 4) |
            ((uint32_t)(error_status == PSP_THREAD_STOPPED) << 5) |
            ((uint32_t)(error_wait_type == 0) << 6) |
             ((uint32_t)((uint32_t)error_exit_status == expected_error_exit) << 7) |
            ((uint32_t)(positive_target >= 0 && positive_start == 0) << 8) |
            ((uint32_t)(positive_joiner >= 0 && positive_join_start == 0) << 9) |
            ((uint32_t)(positive_delete == 0) << 10) |
            ((uint32_t)((uint32_t)positive_inner == 0x800201acu) << 11) |
            ((uint32_t)(positive_ref_result == 0) << 12) |
            ((uint32_t)(positive_status == PSP_THREAD_STOPPED) << 13) |
            ((uint32_t)(positive_wait_type == 0) << 14) |
             ((uint32_t)((uint32_t)positive_exit_status == expected_positive_exit) << 15) |
            ((uint32_t)(error_waiting == 0) << 16) |
            ((uint32_t)(error_done == 0) << 17) |
            ((uint32_t)(positive_waiting == 0) << 18) |
            ((uint32_t)(positive_done == 0) << 19) |
            ((uint32_t)(explicit_exit && (uint32_t)error_outer == expected_error_exit) << 20) |
            ((uint32_t)(explicit_exit && (uint32_t)positive_outer == expected_positive_exit) << 21);
    *out1 = (uint32_t)error_inner;
    *out2 = (uint32_t)error_outer;
    *out3 = (uint32_t)error_ref;
    *out4 = error_status;
    *out5 = error_wait_type;
    *out6 = error_wait_id;
    *out7 = error_exit_status;
    *out8 = (uint32_t)positive_inner;
    *out9 = (uint32_t)positive_outer;
    *out10 = (uint32_t)positive_ref;
    *out11 = positive_status;
    *out12 = positive_wait_type;
    *out13 = positive_wait_id;
    *out14 = positive_exit_status;
    *out15 = boundary ? explicit_error : 0u;
    *out16 = boundary ? positive_exit_argument : 0u;
    return error_target >= 0 && error_start == 0 &&
           error_joiner >= 0 && error_join_start == 0 && error_delete == 0 &&
           error_waiting == 0 && error_done == 0 &&
           (uint32_t)error_inner == 0x800201acu && error_ref_result == 0 &&
           (uint32_t)error_outer == expected_error_exit && error_status == PSP_THREAD_STOPPED &&
           error_wait_type == 0 && error_wait_id == 0 &&
           error_exit_status == expected_error_exit &&
           positive_target >= 0 && positive_start == 0 &&
           positive_joiner >= 0 && positive_join_start == 0 && positive_delete == 0 &&
           positive_waiting == 0 && positive_done == 0 &&
           (uint32_t)positive_inner == 0x800201acu && positive_ref_result == 0 &&
           (uint32_t)positive_outer == expected_positive_exit && positive_status == PSP_THREAD_STOPPED &&
           positive_wait_type == 0 && positive_wait_id == 0 &&
           positive_exit_status == expected_positive_exit;
}
#endif

#if ((PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_CONCURRENCY && \
      PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC) || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0) || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS
#define DMAC_API_MEMCPY 0u
#define DMAC_API_TRY_MEMCPY 1u
#define DMAC_MEASURED_PREFIX 0x0000c000u
#define DMAC_REFERENCE_BUSY 0x80000021u

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY || \
    (PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
     PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC) || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0 || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS
static int dmac_call(uint32_t api, void *dst, const void *src, uint32_t size) {
    return api == DMAC_API_TRY_MEMCPY
        ? sceDmacTryMemcpy(dst, src, size)
        : sceDmacMemcpy(dst, src, size);
}

#if PSP_ORACLE_CASE != PSP_ORACLE_CASE_DMAC_CELLS
static uint8_t dmac_pattern(uint32_t offset) {
    return (uint8_t)(0x10u + (offset & 0x3fu));
}

/* Only the concurrency and size-matrix launches time transfers, so the helper
   is compiled for those launches alone (the invalid-tail launches never use it). */
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL
static uint32_t dmac_elapsed_us(uint64_t start, uint64_t end) {
    const uint64_t elapsed = end >= start ? end - start : 0;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}
#endif /* launches that time transfers (dmac_elapsed_us) */
#endif /* PSP_ORACLE_CASE != PSP_ORACLE_CASE_DMAC_CELLS (dmac_pattern) */
#endif /* launches that issue DMAC copies (dmac_call) */
#endif /* DMAC launches (DMAC_API_* and DMAC_* constants) */

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL
/* Keep each transfer inside two partition-2 blocks owned by this probe.  The
   page-sized redzones also absorb cache-line rounding around the requested
   spans. */
#define DMAC_SIZE_PARTITION 2
#define DMAC_SIZE_ALIGNMENT 0x1000u
#define DMAC_SIZE_REDZONE_BYTES 0x1000u
#define DMAC_SIZE_SENTINEL 0xa5u
#define DMAC_SIZE_SRC_GUARD_BEFORE 0x96u
#define DMAC_SIZE_SRC_GUARD_AFTER 0x69u
#define DMAC_SIZE_DST_GUARD_BEFORE 0x5au
#define DMAC_SIZE_DST_GUARD_AFTER 0xc3u
#define DMAC_SIZE_TRIALS 3u

#if defined(DMAC_SIZE_REQUEST)
#if DMAC_SIZE_REQUEST != 0x0000bfffu && DMAC_SIZE_REQUEST != 0x0000c000u && \
    DMAC_SIZE_REQUEST != 0x0000c001u && DMAC_SIZE_REQUEST != 0x0000d000u && \
    DMAC_SIZE_REQUEST != 0x0000f000u && DMAC_SIZE_REQUEST != 0x0000ffffu && \
    DMAC_SIZE_REQUEST != 0x00010000u && DMAC_SIZE_REQUEST != 0x00100000u
#error DMAC_SIZE_REQUEST must be one of the supported matrix sizes
#endif
static const uint32_t dmac_size_requests[] = { DMAC_SIZE_REQUEST };
#else
static const uint32_t dmac_size_requests[] = {
    0x0000bfffu, 0x0000c000u, 0x0000c001u, 0x0000d000u,
    0x0000f000u, 0x0000ffffu, 0x00010000u, 0x00100000u,
};
#endif

struct dmac_size_buffer {
    SceUID block_uid;
    uint8_t *head;
    uint8_t *data;
    uint32_t allocation_bytes;
};

static void dmac_size_cache_sync(void *address, uint32_t size) {
    sceKernelDcacheWritebackRange(address, size);
    sceKernelDcacheInvalidateRange(address, size);
}

static int dmac_size_allocation_bytes(uint32_t requested, uint32_t *allocation_bytes) {
    const uint32_t overhead = 2u * DMAC_SIZE_REDZONE_BYTES;
    if (requested > UINT32_MAX - overhead - (DMAC_SIZE_ALIGNMENT - 1u)) return 0;
    const uint32_t total = requested + overhead;
    *allocation_bytes =
        (total + DMAC_SIZE_ALIGNMENT - 1u) & ~(DMAC_SIZE_ALIGNMENT - 1u);
    return *allocation_bytes >= total;
}

static int dmac_size_allocate_buffer(struct dmac_size_buffer *buffer,
                                     const char *name, uint32_t requested,
                                     uint32_t *failure) {
    uint32_t allocation_bytes = 0;
    buffer->block_uid = -1;
    buffer->head = NULL;
    buffer->data = NULL;
    buffer->allocation_bytes = 0;
    if (!dmac_size_allocation_bytes(requested, &allocation_bytes)) {
        *failure = UINT32_MAX;
        return 0;
    }

    const SceUID block_uid = sceKernelAllocPartitionMemory(
        DMAC_SIZE_PARTITION, name, PSP_SMEM_High, allocation_bytes, NULL);
    if (block_uid < 0) {
        *failure = (uint32_t)block_uid;
        return 0;
    }
    uint8_t *const head = (uint8_t *)sceKernelGetBlockHeadAddr(block_uid);
    const uintptr_t address = (uintptr_t)head;
    if (!head || address > UINTPTR_MAX - (uintptr_t)allocation_bytes ||
        (address & (DMAC_SIZE_ALIGNMENT - 1u)) != 0u) {
        sceKernelFreePartitionMemory(block_uid);
        *failure = 0;
        return 0;
    }

    buffer->block_uid = block_uid;
    buffer->head = head;
    buffer->data = head + DMAC_SIZE_REDZONE_BYTES;
    buffer->allocation_bytes = allocation_bytes;
    if (((uintptr_t)buffer->data & (DMAC_SIZE_ALIGNMENT - 1u)) != 0u) {
        sceKernelFreePartitionMemory(block_uid);
        buffer->block_uid = -1;
        buffer->head = NULL;
        buffer->data = NULL;
        buffer->allocation_bytes = 0;
        *failure = 0;
        return 0;
    }
    *failure = 0;
    return 1;
}

static void dmac_size_release_buffer(struct dmac_size_buffer *buffer) {
    if (buffer->block_uid >= 0) {
        sceKernelFreePartitionMemory(buffer->block_uid);
        buffer->block_uid = -1;
    }
}

static int dmac_size_spans_overlap(const struct dmac_size_buffer *left,
                                   const struct dmac_size_buffer *right) {
    const uintptr_t left_begin = (uintptr_t)left->head;
    const uintptr_t right_begin = (uintptr_t)right->head;
    if (left_begin > UINTPTR_MAX - (uintptr_t)left->allocation_bytes ||
        right_begin > UINTPTR_MAX - (uintptr_t)right->allocation_bytes) return 1;
    const uintptr_t left_end = left_begin + left->allocation_bytes;
    const uintptr_t right_end = right_begin + right->allocation_bytes;
    return left_begin < right_end && right_begin < left_end;
}

static void dmac_size_reset_source(struct dmac_size_buffer *source,
                                   uint32_t requested) {
    memset(source->head, DMAC_SIZE_SRC_GUARD_BEFORE, DMAC_SIZE_REDZONE_BYTES);
    for (uint32_t offset = 0; offset < requested; ++offset) {
        source->data[offset] = dmac_pattern(offset);
    }
    memset(source->data + requested, DMAC_SIZE_SRC_GUARD_AFTER,
           source->allocation_bytes - DMAC_SIZE_REDZONE_BYTES - requested);
}

static void dmac_size_reset_destination(struct dmac_size_buffer *destination,
                                        uint32_t requested) {
    memset(destination->head, DMAC_SIZE_DST_GUARD_BEFORE,
           DMAC_SIZE_REDZONE_BYTES);
    memset(destination->data, DMAC_SIZE_SENTINEL, requested);
    memset(destination->data + requested, DMAC_SIZE_DST_GUARD_AFTER,
           destination->allocation_bytes - DMAC_SIZE_REDZONE_BYTES - requested);
}

static uint32_t dmac_size_prefix(const uint8_t *destination, uint32_t requested) {
    uint32_t offset = 0;
    while (offset < requested && destination[offset] == dmac_pattern(offset)) ++offset;
    return offset;
}

static uint32_t dmac_size_non_sentinel(const uint8_t *destination,
                                       uint32_t offset, uint32_t requested) {
    uint32_t count = 0;
    while (offset < requested) {
        if (destination[offset] != DMAC_SIZE_SENTINEL) ++count;
        ++offset;
    }
    return count;
}

static uint32_t dmac_size_source_mutations(const struct dmac_size_buffer *source,
                                           uint32_t requested) {
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < requested; ++offset) {
        if (source->data[offset] != dmac_pattern(offset)) ++count;
    }
    return count;
}

static uint32_t dmac_size_guard_mutations(const struct dmac_size_buffer *buffer,
                                          uint32_t requested,
                                          uint8_t guard_before,
                                          uint8_t guard_after,
                                          uint32_t *post_mutations) {
    uint32_t before_count = 0;
    uint32_t after_count = 0;
    for (uint32_t offset = 0; offset < DMAC_SIZE_REDZONE_BYTES; ++offset) {
        if (buffer->head[offset] != guard_before) ++before_count;
    }
    const uint32_t post_start = DMAC_SIZE_REDZONE_BYTES + requested;
    for (uint32_t offset = post_start; offset < buffer->allocation_bytes; ++offset) {
        if (buffer->head[offset] != guard_after) ++after_count;
    }
    *post_mutations = after_count;
    return before_count + after_count;
}

static void dmac_size_emit_setup_skip(int emulated, uint32_t requested,
                                      uint32_t api, uint32_t failure) {
    char case_id[64];
    snprintf(case_id, sizeof(case_id), "size-matrix-%s-0x%08x",
             api == DMAC_API_TRY_MEMCPY ? "try" : "memcpy",
             (unsigned int)requested);
    const uint32_t out[] = {
        requested, 0u, 0u, 0u, 0u, api, 0u, 1u, 0u, 0u, 0u,
        DMAC_SIZE_ALIGNMENT, 0u, DMAC_SIZE_PARTITION, DMAC_SIZE_REDZONE_BYTES,
        0u, 0u, 0u, 0u,
    };
    emit_record_extended(emulated, "PSP-DMAC-001", case_id, "SKIP", failure,
                         out, sizeof(out) / sizeof(out[0]));
}

static void run_dmac_size_matrix(int emulated) {
    const uint32_t api_count = 2u;
    const uint32_t request_count =
        (uint32_t)(sizeof(dmac_size_requests) / sizeof(dmac_size_requests[0]));
    for (uint32_t i = 0; i < request_count; ++i) {
        const uint32_t requested = dmac_size_requests[i];
        struct dmac_size_buffer source_buffer = { -1, NULL, NULL, 0u };
        struct dmac_size_buffer destination_buffer = { -1, NULL, NULL, 0u };
        uint32_t setup_failure = 0;
        if (!dmac_size_allocate_buffer(&source_buffer, "oracle-dmac-size-src",
                                       requested, &setup_failure)) {
            dmac_size_emit_setup_skip(emulated, requested, DMAC_API_MEMCPY,
                                      setup_failure);
            dmac_size_emit_setup_skip(emulated, requested,
                                      DMAC_API_TRY_MEMCPY, setup_failure);
            continue;
        }
        if (!dmac_size_allocate_buffer(&destination_buffer, "oracle-dmac-size-dst",
                                       requested, &setup_failure)) {
            dmac_size_release_buffer(&source_buffer);
            dmac_size_emit_setup_skip(emulated, requested, DMAC_API_MEMCPY,
                                      setup_failure);
            dmac_size_emit_setup_skip(emulated, requested,
                                      DMAC_API_TRY_MEMCPY, setup_failure);
            continue;
        }
        if (dmac_size_spans_overlap(&source_buffer, &destination_buffer)) {
            dmac_size_release_buffer(&destination_buffer);
            dmac_size_release_buffer(&source_buffer);
            dmac_size_emit_setup_skip(emulated, requested, DMAC_API_MEMCPY,
                                      UINT32_MAX);
            dmac_size_emit_setup_skip(emulated, requested,
                                      DMAC_API_TRY_MEMCPY, UINT32_MAX);
            continue;
        }

        const uint32_t source_block_uid = (uint32_t)source_buffer.block_uid;
        const uint32_t destination_block_uid = (uint32_t)destination_buffer.block_uid;
        for (uint32_t api = 0; api < api_count; ++api) {
            uint32_t failed_trials = 0;
            uint32_t max_prefix = 0;
            uint32_t max_stray = 0;
            uint32_t max_source_mutations = 0;
            uint32_t max_source_guard_mutations = 0;
            uint32_t max_destination_post_guard_mutations = 0;
            uint32_t max_destination_pre_guard_mutations = 0;
            uint32_t max_elapsed_us = 0;
            uint32_t last_result = 0;
            for (uint32_t trial = 0; trial < DMAC_SIZE_TRIALS; ++trial) {
                dmac_size_reset_source(&source_buffer, requested);
                dmac_size_reset_destination(&destination_buffer, requested);
                dmac_size_cache_sync(source_buffer.head,
                                     source_buffer.allocation_bytes);
                dmac_size_cache_sync(destination_buffer.head,
                                     destination_buffer.allocation_bytes);
                const uint64_t start_us = sceKernelGetSystemTimeWide();
                last_result = (uint32_t)dmac_call(
                    api, destination_buffer.data, source_buffer.data, requested);
                const uint64_t end_us = sceKernelGetSystemTimeWide();
                sceKernelDcacheInvalidateRange(destination_buffer.head,
                                               destination_buffer.allocation_bytes);
                sceKernelDcacheInvalidateRange(source_buffer.head,
                                               source_buffer.allocation_bytes);

                const uint32_t prefix = dmac_size_prefix(destination_buffer.data,
                                                         requested);
                const uint32_t stray = dmac_size_non_sentinel(
                    destination_buffer.data, prefix, requested);
                const uint32_t source_mutations = dmac_size_source_mutations(
                    &source_buffer, requested);
                uint32_t source_post_guard_mutations = 0;
                const uint32_t source_guard_mutations = dmac_size_guard_mutations(
                    &source_buffer, requested, DMAC_SIZE_SRC_GUARD_BEFORE,
                    DMAC_SIZE_SRC_GUARD_AFTER, &source_post_guard_mutations);
                uint32_t destination_post_guard_mutations = 0;
                const uint32_t destination_guard_mutations =
                    dmac_size_guard_mutations(&destination_buffer, requested,
                                              DMAC_SIZE_DST_GUARD_BEFORE,
                                              DMAC_SIZE_DST_GUARD_AFTER,
                                              &destination_post_guard_mutations);
                const uint32_t destination_pre_guard_mutations =
                    destination_guard_mutations - destination_post_guard_mutations;
                const uint32_t elapsed_us = dmac_elapsed_us(start_us, end_us);
                if (prefix > max_prefix) max_prefix = prefix;
                if (stray > max_stray) max_stray = stray;
                if (source_mutations > max_source_mutations) {
                    max_source_mutations = source_mutations;
                }
                if (source_guard_mutations > max_source_guard_mutations) {
                    max_source_guard_mutations = source_guard_mutations;
                }
                if (destination_post_guard_mutations >
                    max_destination_post_guard_mutations) {
                    max_destination_post_guard_mutations =
                        destination_post_guard_mutations;
                }
                if (destination_pre_guard_mutations >
                    max_destination_pre_guard_mutations) {
                    max_destination_pre_guard_mutations =
                        destination_pre_guard_mutations;
                }
                if (elapsed_us > max_elapsed_us) max_elapsed_us = elapsed_us;
                if (last_result != 0u || prefix != requested || stray != 0u ||
                    source_mutations != 0u || source_guard_mutations != 0u ||
                    destination_pre_guard_mutations != 0u ||
                    destination_post_guard_mutations != 0u) {
                    ++failed_trials;
                }
            }
            char case_id[64];
            snprintf(case_id, sizeof(case_id), "size-matrix-%s-0x%08x",
                     api == DMAC_API_TRY_MEMCPY ? "try" : "memcpy",
                     (unsigned int)requested);
            const uint32_t out[] = {
                requested,
                max_prefix,
                max_stray,
                max_source_mutations == 0u ? 1u : 0u,
                max_elapsed_us,
                api,
                DMAC_SIZE_TRIALS,
                failed_trials,
                max_source_guard_mutations,
                max_destination_post_guard_mutations,
                max_destination_pre_guard_mutations,
                DMAC_SIZE_ALIGNMENT,
                source_buffer.allocation_bytes,
                DMAC_SIZE_PARTITION,
                DMAC_SIZE_REDZONE_BYTES,
                (uint32_t)(uintptr_t)source_buffer.data,
                (uint32_t)(uintptr_t)destination_buffer.data,
                source_block_uid,
                destination_block_uid,
            };
            emit_record_extended(emulated, "PSP-DMAC-001", case_id,
                                 failed_trials == 0u ? "PASS" : "FAIL",
                                 last_result, out, sizeof(out) / sizeof(out[0]));
        }
        dmac_size_release_buffer(&destination_buffer);
        dmac_size_release_buffer(&source_buffer);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
#define DMAC_CONCURRENCY_BYTES 0x00100000u
#define DMAC_CONCURRENCY_TRIALS 64u
#define DMAC_FIRST_PRIORITY 0x10u
#define DMAC_DEST_SENTINEL 0xa5u
#define DMAC_CONCURRENCY_DST ((uint8_t *)0x04000000u)
#define DMAC_CONCURRENCY_SRC ((uint8_t *)0x04100000u)

static volatile uint32_t s_dmac_first_api;
static volatile uint32_t s_dmac_first_entered;
static volatile uint32_t s_dmac_first_returned;
static volatile uint32_t s_dmac_first_result;
static volatile uint64_t s_dmac_first_enter_us;
static volatile uint64_t s_dmac_first_exit_us;

static int dmac_first_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    s_dmac_first_enter_us = sceKernelGetSystemTimeWide();
    s_dmac_first_entered = 1;
    s_dmac_first_result = (uint32_t)dmac_call(
        s_dmac_first_api, DMAC_CONCURRENCY_DST,
        DMAC_CONCURRENCY_SRC, DMAC_CONCURRENCY_BYTES);
    s_dmac_first_exit_us = sceKernelGetSystemTimeWide();
    s_dmac_first_returned = 1;
    return 0;
}

static uint32_t dmac_contiguous_prefix(void) {
    uint32_t offset = 0;
    while (offset < DMAC_CONCURRENCY_BYTES &&
           DMAC_CONCURRENCY_DST[offset] == dmac_pattern(offset)) {
        ++offset;
    }
    return offset;
}

static uint32_t dmac_non_sentinel_after(uint32_t offset) {
    uint32_t count = 0;
    while (offset < DMAC_CONCURRENCY_BYTES) {
        if (DMAC_CONCURRENCY_DST[offset] != DMAC_DEST_SENTINEL) ++count;
        ++offset;
    }
    return count;
}

static void run_dmac_concurrency_combo(int emulated, uint32_t first_api,
                                       uint32_t second_api,
                                       const char *case_id) {
    uint32_t attempted = 0;
    uint32_t first_entered_count = 0;
    uint32_t first_returned_count = 0;
    uint32_t start_window_count = 0;
    uint32_t timeline_overlap_count = 0;
    uint32_t first_zero_count = 0;
    uint32_t first_other_count = 0;
    uint32_t second_busy_count = 0;
    uint32_t second_zero_count = 0;
    uint32_t second_other_count = 0;
    uint32_t busy_while_first_pending_count = 0;
    uint32_t busy_after_first_return_count = 0;
    uint32_t prefix_min = UINT32_MAX;
    uint32_t prefix_max = 0;
    uint32_t prefix_c000_count = 0;
    uint32_t stray_mutation_count = 0;
    uint32_t first_min_us = UINT32_MAX;
    uint32_t first_max_us = 0;
    uint32_t second_min_us = UINT32_MAX;
    uint32_t second_max_us = 0;
    uint32_t last_first_result = 0;
    uint32_t last_second_result = 0;
    uint32_t setup_error = 0;

    for (uint32_t offset = 0; offset < DMAC_CONCURRENCY_BYTES; ++offset) {
        DMAC_CONCURRENCY_SRC[offset] = dmac_pattern(offset);
    }
    sceKernelDcacheWritebackInvalidateRange(
        DMAC_CONCURRENCY_SRC, DMAC_CONCURRENCY_BYTES);

    for (uint32_t trial = 0; trial < DMAC_CONCURRENCY_TRIALS; ++trial) {
        memset(DMAC_CONCURRENCY_DST, DMAC_DEST_SENTINEL,
               DMAC_CONCURRENCY_BYTES);
        sceKernelDcacheWritebackInvalidateRange(
            DMAC_CONCURRENCY_DST, DMAC_CONCURRENCY_BYTES);

        s_dmac_first_api = first_api;
        s_dmac_first_entered = 0;
        s_dmac_first_returned = 0;
        s_dmac_first_result = 0;
        s_dmac_first_enter_us = 0;
        s_dmac_first_exit_us = 0;

        const SceUID thread = sceKernelCreateThread(
            "oracle-dmac-first", dmac_first_thread, DMAC_FIRST_PRIORITY,
            0x2000, 0, NULL);
        if (thread < 0) {
            setup_error = (uint32_t)thread;
            break;
        }
        const int start_result = sceKernelStartThread(thread, 0, NULL);
        if (start_result < 0) {
            setup_error = (uint32_t)start_result;
            sceKernelDeleteThread(thread);
            break;
        }

        /* A high-priority first caller can return control here only if the
           syscall blocks/yields or has already returned.  Record that state;
           do not infer an in-flight DMA solely from thread scheduling. */
        const uint32_t entered_before_second = s_dmac_first_entered;
        const uint32_t returned_before_second = s_dmac_first_returned;
        if (entered_before_second) ++first_entered_count;
        if (returned_before_second) ++first_returned_count;
        if (entered_before_second && !returned_before_second) {
            ++start_window_count;
        }

        const uint64_t second_enter_us = sceKernelGetSystemTimeWide();
        const uint32_t second_result = (uint32_t)dmac_call(
            second_api, DMAC_CONCURRENCY_DST,
            DMAC_CONCURRENCY_SRC, DMAC_CONCURRENCY_BYTES);
        const uint64_t second_exit_us = sceKernelGetSystemTimeWide();
        (void)sceKernelWaitThreadEnd(thread, NULL);
        sceKernelDeleteThread(thread);
        ++attempted;

        const uint32_t first_result = s_dmac_first_result;
        const uint32_t first_us = dmac_elapsed_us(
            s_dmac_first_enter_us, s_dmac_first_exit_us);
        const uint32_t second_us = dmac_elapsed_us(
            second_enter_us, second_exit_us);
        if (second_enter_us < s_dmac_first_exit_us) ++timeline_overlap_count;
        if (first_result == 0) ++first_zero_count; else ++first_other_count;
        if (second_result == DMAC_REFERENCE_BUSY) {
            ++second_busy_count;
            if (entered_before_second && !returned_before_second) {
                ++busy_while_first_pending_count;
            }
            if (returned_before_second) {
                ++busy_after_first_return_count;
            }
        } else if (second_result == 0) {
            ++second_zero_count;
        } else {
            ++second_other_count;
        }
        if (first_us < first_min_us) first_min_us = first_us;
        if (first_us > first_max_us) first_max_us = first_us;
        if (second_us < second_min_us) second_min_us = second_us;
        if (second_us > second_max_us) second_max_us = second_us;
        last_first_result = first_result;
        last_second_result = second_result;

        sceKernelDcacheInvalidateRange(
            DMAC_CONCURRENCY_DST, DMAC_CONCURRENCY_BYTES);
        const uint32_t prefix = dmac_contiguous_prefix();
        if (prefix < prefix_min) prefix_min = prefix;
        if (prefix > prefix_max) prefix_max = prefix;
        if (prefix == DMAC_MEASURED_PREFIX) ++prefix_c000_count;
        if (dmac_non_sentinel_after(prefix) != 0) ++stray_mutation_count;
    }

    if (attempted == 0) {
        prefix_min = 0;
        first_min_us = 0;
        second_min_us = 0;
    }
    const uint32_t out[] = {
        attempted,
        first_api,
        second_api,
        first_entered_count,
        first_returned_count,
        start_window_count,
        timeline_overlap_count,
        first_zero_count,
        first_other_count,
        second_busy_count,
        second_zero_count,
        second_other_count,
        busy_while_first_pending_count,
        busy_after_first_return_count,
        prefix_min,
        prefix_max,
        prefix_c000_count,
        stray_mutation_count,
        first_min_us,
        first_max_us,
        second_min_us,
        second_max_us,
        DMAC_FIRST_PRIORITY,
        last_first_result,
    };
    emit_record_extended(emulated, "PSP-DMAC-001", case_id,
                         setup_error ? "ERROR" : "PASS",
                         setup_error ? setup_error : last_second_result,
                         out, sizeof(out) / sizeof(out[0]));
}

static void run_dmac_concurrency(int emulated) {
    /* This reproduces the scheduling shape in PSPAutotests' public DMAC test,
       then adds a Try/Try control and a normal-call second caller.  The joint
       scalar state distinguishes BUSY while the first caller remains pending
       from BUSY after that syscall returned. Zero BUSY returns without a
       first-caller window are merely "not measurable with this probe." */
    run_dmac_concurrency_combo(emulated, DMAC_API_MEMCPY,
                               DMAC_API_TRY_MEMCPY,
                               "concurrent-memcpy-try");
    run_dmac_concurrency_combo(emulated, DMAC_API_TRY_MEMCPY,
                               DMAC_API_TRY_MEMCPY,
                               "concurrent-try-try");
    run_dmac_concurrency_combo(emulated, DMAC_API_TRY_MEMCPY,
                               DMAC_API_MEMCPY,
                               "concurrent-try-memcpy");
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
    (PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC || \
     PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0)

#define DMAC_INVALID_PARTITION 2
#define DMAC_INVALID_PAYLOAD_OFFSET DMAC_INVALID_PRE_GUARD_BYTES
#define DMAC_INVALID_POST_OFFSET \
    (DMAC_INVALID_PAYLOAD_OFFSET + DMAC_INVALID_PAYLOAD_BYTES)
#define DMAC_INVALID_OVERFLOW_OFFSET \
    (DMAC_INVALID_POST_OFFSET + DMAC_INVALID_POST_GUARD_BYTES)
#define DMAC_INVALID_TAIL_OFFSET \
    (DMAC_INVALID_OVERFLOW_OFFSET + DMAC_INVALID_OVERFLOW_BAND_BYTES)
#define DMAC_INVALID_SETUP_MASK 0x0fu
#define DMAC_INVALID_SETUP_BLOCK 0x01u
#define DMAC_INVALID_SETUP_GEOMETRY 0x02u
#define DMAC_INVALID_SETUP_END_NEIGHBOR 0x04u
#define DMAC_INVALID_SETUP_BEGIN_NEIGHBOR 0x08u
#define DMAC_INVALID_NEIGHBOR_BYTES 0x1000u

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST
#define DMAC_INVALID_API DMAC_API_MEMCPY
#define DMAC_INVALID_API_NAME "memcpy"
#define DMAC_INVALID_ENDPOINT "dst"
#define DMAC_INVALID_B_CELL "b1"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_SRC
#define DMAC_INVALID_API DMAC_API_MEMCPY
#define DMAC_INVALID_API_NAME "memcpy"
#define DMAC_INVALID_ENDPOINT "src"
#define DMAC_INVALID_B_CELL "b2"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_DST
#define DMAC_INVALID_API DMAC_API_TRY_MEMCPY
#define DMAC_INVALID_API_NAME "try"
#define DMAC_INVALID_ENDPOINT "dst"
#define DMAC_INVALID_B_CELL "b3"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
#define DMAC_INVALID_API DMAC_API_TRY_MEMCPY
#define DMAC_INVALID_API_NAME "try"
#define DMAC_INVALID_ENDPOINT "src"
#define DMAC_INVALID_B_CELL "b4"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0
#define DMAC_INVALID_API DMAC_API_MEMCPY
#define DMAC_INVALID_API_NAME "memcpy"
#define DMAC_INVALID_ENDPOINT "dst"
#define DMAC_INVALID_B_CELL "b1"
#endif

static uint8_t s_dmac_invalid_io[DMAC_INVALID_MAX_REQUEST]
    __attribute__((aligned(64)));

static void dmac_cell_cache_before(void *ptr, uint32_t size) {
    sceKernelDcacheWritebackInvalidateRange(ptr, size);
}

static void dmac_cell_cache_after(void *ptr, uint32_t size) {
    sceKernelDcacheInvalidateRange(ptr, size);
}

static void dmac_invalid_fill_scratch(uint8_t *head) {
    memset(head, DMAC_INVALID_PRE_GUARD_FILL, DMAC_INVALID_PRE_GUARD_BYTES);
    memset(head + DMAC_INVALID_PAYLOAD_OFFSET, DMAC_INVALID_PAYLOAD_FILL,
           DMAC_INVALID_PAYLOAD_BYTES);
    memset(head + DMAC_INVALID_POST_OFFSET, DMAC_INVALID_POST_GUARD_FILL,
           DMAC_INVALID_POST_GUARD_BYTES);
    memset(head + DMAC_INVALID_OVERFLOW_OFFSET,
           DMAC_INVALID_OVERFLOW_BAND_FILL,
           DMAC_INVALID_OVERFLOW_BAND_BYTES);
    memset(head + DMAC_INVALID_TAIL_OFFSET, DMAC_INVALID_TAIL_GUARD_FILL,
           DMAC_INVALID_TAIL_GUARD_BYTES);
}

static uint32_t dmac_invalid_region_mutations(const uint8_t *bytes,
                                              uint32_t offset,
                                              uint32_t size,
                                              uint8_t expected) {
    uint32_t mutations = 0;
    for (uint32_t index = 0; index < size; ++index) {
        if (bytes[offset + index] != expected) ++mutations;
    }
    return mutations;
}

static uint8_t dmac_invalid_expected_source_byte(uint32_t offset) {
    if (offset < DMAC_INVALID_PAYLOAD_BYTES + DMAC_INVALID_POST_GUARD_BYTES) {
        return DMAC_INVALID_PAYLOAD_FILL;
    }
    return DMAC_INVALID_OVERFLOW_BAND_FILL;
}

static void dmac_invalid_fill_io_source(uint32_t size) {
    for (uint32_t index = 0; index < size; ++index) {
        s_dmac_invalid_io[index] = dmac_pattern(index);
    }
}

static uint32_t dmac_invalid_io_source_intact(uint32_t size) {
    for (uint32_t index = 0; index < size; ++index) {
        if (s_dmac_invalid_io[index] != dmac_pattern(index)) return 0u;
    }
    return 1u;
}

static void dmac_invalid_emit_record(int emulated, const char *case_id,
                                     const char *status, uint32_t rc,
                                     uint32_t prefix, uint32_t matches,
                                     uint32_t guards_outside,
                                     uint32_t post_guard,
                                     uint32_t overflow_band,
                                     uint32_t source_intact,
                                     uint32_t setup_mask, uint32_t delta,
                                     const char *api, const char *endpoint,
                                     uint32_t cache_discipline,
                                     const char *tier, uint32_t executed,
                                     uint32_t payload_mutations,
                                     uint32_t source_addr,
                                     uint32_t destination_addr) {
    char line[768];
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
             "case_id=%s status=%s result=0x%08x rc=0x%08x "
             "P=0x%08x matches=0x%08x guards_outside=0x%08x "
             "post_guard=0x%08x overflow_band=0x%08x "
             "source_intact=0x%08x setup_mask=0x%08x K=0x%08x "
             "delta=0x%08x api=%s endpoint=%s cache_discipline=0x%08x "
             "tier=%s executed=0x%08x payload_mutations=0x%08x "
             "source_addr=0x%08x destination_addr=0x%08x\n",
             case_id, status, (unsigned int)rc, (unsigned int)rc,
             (unsigned int)prefix, (unsigned int)matches,
             (unsigned int)guards_outside, (unsigned int)post_guard,
             (unsigned int)overflow_band, (unsigned int)source_intact,
             (unsigned int)setup_mask, (unsigned int)DMAC_INVALID_PAYLOAD_BYTES,
             (unsigned int)delta, api, endpoint,
             (unsigned int)cache_discipline, tier, (unsigned int)executed,
             (unsigned int)payload_mutations, (unsigned int)source_addr,
             (unsigned int)destination_addr);
    probe_emit_durable(emulated, line, strlen(line));
}

static int dmac_invalid_probe_neighbor(const char *name, uint8_t *address) {
    const SceUID neighbor = sceKernelAllocPartitionMemory(
        DMAC_INVALID_PARTITION, name, PSP_SMEM_Addr,
        DMAC_INVALID_NEIGHBOR_BYTES, address);
    if (neighbor < 0) return 1;
    sceKernelFreePartitionMemory(neighbor);
    return 0;
}

static int dmac_invalid_acquire(uint32_t *setup_mask, uint32_t *setup_error,
                                SceUID *block_uid, uint8_t **block_head) {
    *setup_mask = 0;
    *setup_error = 0;
    *block_uid = sceKernelAllocPartitionMemory(
        DMAC_INVALID_PARTITION, "oracle-dmac-scratch", PSP_SMEM_High,
        DMAC_INVALID_SCRATCH_BYTES, NULL);
    if (*block_uid < 0) {
        *setup_error = (uint32_t)*block_uid;
        return 0;
    }
    *setup_mask |= DMAC_INVALID_SETUP_BLOCK;
    *block_head = (uint8_t *)sceKernelGetBlockHeadAddr(*block_uid);
    if (!*block_head || ((uintptr_t)*block_head & 0xfffu) != 0u ||
        (uintptr_t)*block_head < DMAC_INVALID_NEIGHBOR_BYTES ||
        (uintptr_t)*block_head > UINTPTR_MAX - DMAC_INVALID_SCRATCH_BYTES) {
        *setup_error = 1u;
        sceKernelFreePartitionMemory(*block_uid);
        *block_uid = -1;
        return 0;
    }
    *setup_mask |= DMAC_INVALID_SETUP_GEOMETRY;

    uint8_t *const block_end = *block_head + DMAC_INVALID_SCRATCH_BYTES;
    if (!dmac_invalid_probe_neighbor("oracle-dmac-neighbor-end", block_end)) {
        *setup_error = 1u;
        sceKernelFreePartitionMemory(*block_uid);
        *block_uid = -1;
        return 0;
    }
    *setup_mask |= DMAC_INVALID_SETUP_END_NEIGHBOR;
    if (!dmac_invalid_probe_neighbor(
            "oracle-dmac-neighbor-begin",
            *block_head - DMAC_INVALID_NEIGHBOR_BYTES)) {
        *setup_error = 1u;
        sceKernelFreePartitionMemory(*block_uid);
        *block_uid = -1;
        return 0;
    }
    *setup_mask |= DMAC_INVALID_SETUP_BEGIN_NEIGHBOR;
    return 1;
}

static void dmac_invalid_emit_skips(int emulated, uint32_t setup_mask,
                                    uint32_t setup_error) {
    static const char *const shapes[] = {"a", "b", "c", "d"};
    if (PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0) {
        for (uint32_t api = 0; api < 2u; ++api) {
            const char *const api_name = api == 0u ? "memcpy" : "try";
            for (uint32_t shape = 0; shape < 4u; ++shape) {
                char case_id[48];
                snprintf(case_id, sizeof(case_id), "invalid-tail-s0-%s-%s",
                         shapes[shape], api_name);
                dmac_invalid_emit_record(
                    emulated, case_id, "SKIP", setup_error, 0u, 0u,
                    0u, 0u, 0u, 0u, setup_mask, 0u, api_name,
                    shape == 0u || shape == 3u ? "dst" :
                        (shape == 1u ? "src" : "both"),
                    0u, "S", 0u, 0u, 0u, 0u);
            }
        }
        return;
    }
    static const uint32_t deltas[] = {1u, 4u, 0x1000u, 0x2000u};
    for (uint32_t index = 0; index < sizeof(deltas) / sizeof(deltas[0]); ++index) {
        char case_id[48];
        snprintf(case_id, sizeof(case_id), "invalid-tail-%s-delta-%04x",
                 DMAC_INVALID_B_CELL, (unsigned int)deltas[index]);
        dmac_invalid_emit_record(emulated, case_id, "SKIP", setup_error,
                                 0u, 0u, 0u, 0u, 0u, 0u, setup_mask,
                                 deltas[index], DMAC_INVALID_API_NAME,
                                 DMAC_INVALID_ENDPOINT, 0u, "B", 0u, 0u,
                                 0u, 0u);
    }
}

static void run_dmac_invalid_tier_s(int emulated, uint8_t *block_head,
                                    uint32_t setup_mask) {
    static const char *const shapes[] = {"a", "b", "c", "d"};
    for (uint32_t api = 0; api < 2u; ++api) {
        const char *const api_name = api == 0u ? "memcpy" : "try";
        for (uint32_t shape = 0; shape < 4u; ++shape) {
            uint8_t *const payload = block_head + DMAC_INVALID_PAYLOAD_OFFSET;
            void *dst = payload;
            const void *src = s_dmac_invalid_io;
            const char *const endpoint = shape == 0u || shape == 3u ? "dst" :
                (shape == 1u ? "src" : "both");
            if (shape == 0u) dst = NULL;
            if (shape == 1u) src = NULL;
            if (shape == 3u) dst = (void *)(uintptr_t)UINT32_MAX;

            dmac_invalid_fill_scratch(block_head);
            dmac_invalid_fill_io_source(DMAC_INVALID_MAX_REQUEST);
            dmac_cell_cache_before(block_head, DMAC_INVALID_SCRATCH_BYTES);
            dmac_cell_cache_before(s_dmac_invalid_io, DMAC_INVALID_MAX_REQUEST);
            const int rc = dmac_call(api, dst, src, 0u);
            dmac_cell_cache_after(block_head, DMAC_INVALID_SCRATCH_BYTES);
            dmac_cell_cache_after(s_dmac_invalid_io, DMAC_INVALID_MAX_REQUEST);

            const uint32_t guards_outside =
                dmac_invalid_region_mutations(
                    block_head, 0u, DMAC_INVALID_PRE_GUARD_BYTES,
                    DMAC_INVALID_PRE_GUARD_FILL) +
                dmac_invalid_region_mutations(
                    block_head, DMAC_INVALID_TAIL_OFFSET,
                    DMAC_INVALID_TAIL_GUARD_BYTES,
                    DMAC_INVALID_TAIL_GUARD_FILL);
            const uint32_t post_guard = dmac_invalid_region_mutations(
                block_head, DMAC_INVALID_POST_OFFSET,
                DMAC_INVALID_POST_GUARD_BYTES,
                DMAC_INVALID_POST_GUARD_FILL);
            const uint32_t overflow_band = dmac_invalid_region_mutations(
                block_head, DMAC_INVALID_OVERFLOW_OFFSET,
                DMAC_INVALID_OVERFLOW_BAND_BYTES,
                DMAC_INVALID_OVERFLOW_BAND_FILL);
            const uint32_t payload_mutations = dmac_invalid_region_mutations(
                block_head, DMAC_INVALID_PAYLOAD_OFFSET,
                DMAC_INVALID_PAYLOAD_BYTES, DMAC_INVALID_PAYLOAD_FILL);
            const uint32_t source_intact = dmac_invalid_io_source_intact(
                DMAC_INVALID_MAX_REQUEST);
            const char *const status = guards_outside == 0u && post_guard == 0u &&
                overflow_band == 0u && payload_mutations == 0u &&
                source_intact != 0u ? "PASS" : "FAIL";
            char case_id[48];
            snprintf(case_id, sizeof(case_id), "invalid-tail-s0-%s-%s",
                     shapes[shape], api_name);
            dmac_invalid_emit_record(
                emulated, case_id, status, (uint32_t)rc, 0u, 0u,
                guards_outside, post_guard, overflow_band, source_intact,
                setup_mask, 0u, api_name, endpoint, 1u, "S", 1u,
                payload_mutations, (uint32_t)(uintptr_t)src,
                (uint32_t)(uintptr_t)dst);
        }
    }
}

static void run_dmac_invalid_tier_b(int emulated, uint8_t *block_head,
                                    uint32_t setup_mask) {
    static const uint32_t deltas[] = {1u, 4u, 0x1000u, 0x2000u};
    uint8_t *const payload = block_head + DMAC_INVALID_PAYLOAD_OFFSET;
    for (uint32_t delta_index = 0;
         delta_index < sizeof(deltas) / sizeof(deltas[0]); ++delta_index) {
        const uint32_t delta = deltas[delta_index];
        const uint32_t requested = DMAC_INVALID_PAYLOAD_BYTES + delta;
        void *dst;
        const void *src;
        dmac_invalid_fill_scratch(block_head);
        if (DMAC_INVALID_ENDPOINT[0] == 'd') {
            dmac_invalid_fill_io_source(DMAC_INVALID_MAX_REQUEST);
            dst = payload;
            src = s_dmac_invalid_io;
            dmac_cell_cache_before(s_dmac_invalid_io, requested);
            dmac_cell_cache_before(block_head, DMAC_INVALID_SCRATCH_BYTES);
        } else {
            memset(s_dmac_invalid_io, 0xeeu, requested);
            dst = s_dmac_invalid_io;
            src = payload;
            dmac_cell_cache_before(block_head, DMAC_INVALID_SCRATCH_BYTES);
            dmac_cell_cache_before(s_dmac_invalid_io, requested);
        }
        const int rc = dmac_call(DMAC_INVALID_API, dst, src, requested);
        dmac_cell_cache_after(block_head, DMAC_INVALID_SCRATCH_BYTES);
        dmac_cell_cache_after(s_dmac_invalid_io, requested);

        uint32_t prefix = 0;
        uint32_t matches = 0;
        uint32_t source_intact = 1u;
        for (uint32_t offset = 0; offset < requested; ++offset) {
            const uint8_t expected = DMAC_INVALID_ENDPOINT[0] == 'd'
                ? dmac_pattern(offset)
                : dmac_invalid_expected_source_byte(offset);
            const uint8_t observed = DMAC_INVALID_ENDPOINT[0] == 'd'
                ? payload[offset] : s_dmac_invalid_io[offset];
            if (observed == expected) ++matches;
            if (prefix == offset && observed == expected) ++prefix;
            if (DMAC_INVALID_ENDPOINT[0] == 'd' &&
                s_dmac_invalid_io[offset] != dmac_pattern(offset)) {
                source_intact = 0u;
            }
            if (DMAC_INVALID_ENDPOINT[0] == 's' &&
                payload[offset] != dmac_invalid_expected_source_byte(offset)) {
                source_intact = 0u;
            }
        }
        const uint32_t guards_outside =
            dmac_invalid_region_mutations(
                block_head, 0u, DMAC_INVALID_PRE_GUARD_BYTES,
                DMAC_INVALID_PRE_GUARD_FILL) +
            dmac_invalid_region_mutations(
                block_head, DMAC_INVALID_TAIL_OFFSET,
                DMAC_INVALID_TAIL_GUARD_BYTES, DMAC_INVALID_TAIL_GUARD_FILL);
        const uint32_t post_guard = dmac_invalid_region_mutations(
            block_head, DMAC_INVALID_POST_OFFSET,
            DMAC_INVALID_POST_GUARD_BYTES, DMAC_INVALID_POST_GUARD_FILL);
        const uint32_t overflow_band = dmac_invalid_region_mutations(
            block_head, DMAC_INVALID_OVERFLOW_OFFSET,
            DMAC_INVALID_OVERFLOW_BAND_BYTES, DMAC_INVALID_OVERFLOW_BAND_FILL);
        const uint32_t payload_mutations = DMAC_INVALID_ENDPOINT[0] == 's'
            ? dmac_invalid_region_mutations(
                block_head, DMAC_INVALID_PAYLOAD_OFFSET,
                DMAC_INVALID_PAYLOAD_BYTES, DMAC_INVALID_PAYLOAD_FILL)
            : 0u;
        const int safe = guards_outside == 0u && source_intact != 0u &&
            (DMAC_INVALID_ENDPOINT[0] == 'd' ||
             (post_guard == 0u && overflow_band == 0u &&
              payload_mutations == 0u));
        char case_id[48];
        snprintf(case_id, sizeof(case_id), "invalid-tail-%s-delta-%04x",
                 DMAC_INVALID_B_CELL, (unsigned int)delta);
        dmac_invalid_emit_record(
            emulated, case_id, safe ? "PASS" : "FAIL", (uint32_t)rc,
            prefix, matches, guards_outside, post_guard, overflow_band,
            source_intact, setup_mask, delta, DMAC_INVALID_API_NAME,
            DMAC_INVALID_ENDPOINT, 1u, "B", 1u, payload_mutations,
            (uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst);
    }
}

static void run_dmac_invalid_tail(int emulated) {
    uint32_t setup_mask = 0;
    uint32_t setup_error = 0;
    SceUID block_uid = -1;
    uint8_t *block_head = NULL;
    if (!dmac_invalid_acquire(&setup_mask, &setup_error,
                              &block_uid, &block_head)) {
        dmac_invalid_emit_skips(emulated, setup_mask, setup_error);
        return;
    }
    if (PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0) {
        run_dmac_invalid_tier_s(emulated, block_head, setup_mask);
    } else {
        run_dmac_invalid_tier_b(emulated, block_head, setup_mask);
    }
    sceKernelFreePartitionMemory(block_uid);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS
#define DMAC_CELL_BYTES 64u
#define DMAC_CELL_SENTINEL 0xa5u
#define DMAC_CELL_BASE 32u

static const uint8_t s_dmac_cell_offsets[] = {
    1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u, 11u, 13u, 14u, 15u,
};
static const uint8_t s_dmac_overlap_offsets[] = {1u, 2u, 3u, 7u, 15u};
static uint8_t s_dmac_cell_src[128] __attribute__((aligned(64)));
static uint8_t s_dmac_cell_dst[128] __attribute__((aligned(64)));
static uint8_t s_dmac_cell_overlap[256] __attribute__((aligned(64)));

static uint32_t dmac_cell_prefix_matches(const uint8_t *dst,
                                         const uint8_t *src,
                                         uint32_t size) {
    uint32_t prefix = 0;
    while (prefix < size && dst[prefix] == src[prefix]) ++prefix;
    return prefix;
}

static uint32_t dmac_cell_matches(const uint8_t *dst, const uint8_t *src,
                                  uint32_t size) {
    uint32_t matches = 0;
    for (uint32_t i = 0; i < size; ++i) matches += dst[i] == src[i];
    return matches;
}

static uint32_t dmac_cell_guard_mutations(const uint8_t *before,
                                          const uint8_t *after,
                                          uint32_t total,
                                          uint32_t start,
                                          uint32_t size) {
    uint32_t mutations = 0;
    for (uint32_t i = 0; i < total; ++i) {
        if ((i < start || i >= start + size) && before[i] != after[i]) {
            ++mutations;
        }
    }
    return mutations;
}

static uint32_t dmac_cell_hash(const uint8_t *bytes, uint32_t size) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

static void dmac_cell_cache_before(void *ptr, uint32_t size) {
    sceKernelDcacheWritebackInvalidateRange(ptr, size);
}

static void dmac_cell_cache_after(void *ptr, uint32_t size) {
    sceKernelDcacheInvalidateRange(ptr, size);
}

static void run_dmac_alignment_cell(uint32_t api,
                                    const char *api_name, const char *side,
                                    uint32_t src_offset, uint32_t dst_offset) {
    uint8_t src_before[sizeof(s_dmac_cell_src)];
    uint8_t dst_before[sizeof(s_dmac_cell_dst)];
    const uint32_t src_start = DMAC_CELL_BASE + src_offset;
    const uint32_t dst_start = DMAC_CELL_BASE + dst_offset;
    for (uint32_t i = 0; i < sizeof(s_dmac_cell_src); ++i) {
        s_dmac_cell_src[i] = (uint8_t)(0x31u + (i * 17u));
        s_dmac_cell_dst[i] = DMAC_CELL_SENTINEL;
    }
    memcpy(src_before, s_dmac_cell_src, sizeof(src_before));
    memcpy(dst_before, s_dmac_cell_dst, sizeof(dst_before));
    dmac_cell_cache_before(s_dmac_cell_src, sizeof(s_dmac_cell_src));
    dmac_cell_cache_before(s_dmac_cell_dst, sizeof(s_dmac_cell_dst));

    const int rc = dmac_call(api, &s_dmac_cell_dst[dst_start],
                             &s_dmac_cell_src[src_start], DMAC_CELL_BYTES);
    dmac_cell_cache_after(s_dmac_cell_src, sizeof(s_dmac_cell_src));
    dmac_cell_cache_after(s_dmac_cell_dst, sizeof(s_dmac_cell_dst));

    const uint32_t prefix = dmac_cell_prefix_matches(
        &s_dmac_cell_dst[dst_start], &src_before[src_start], DMAC_CELL_BYTES);
    const uint32_t matches = dmac_cell_matches(
        &s_dmac_cell_dst[dst_start], &src_before[src_start], DMAC_CELL_BYTES);
    const uint32_t guards = dmac_cell_guard_mutations(
        dst_before, s_dmac_cell_dst, sizeof(s_dmac_cell_dst),
        dst_start, DMAC_CELL_BYTES);
    const uint32_t source_intact =
        memcmp(src_before, s_dmac_cell_src, sizeof(src_before)) == 0;
    uint32_t out[10] = {
        api, src_offset, dst_offset, DMAC_CELL_BYTES, prefix, matches,
        guards, source_intact,
        (uint32_t)(uintptr_t)&s_dmac_cell_src[src_start],
        (uint32_t)(uintptr_t)&s_dmac_cell_dst[dst_start],
    };
    char case_id[48];
    snprintf(case_id, sizeof(case_id), "align-%s-%s-%02x",
             api_name, side,
             (unsigned int)(src_offset ? src_offset : dst_offset));
    defer_record(case_id, guards == 0u && source_intact ? "PASS" : "FAIL",
                 (uint32_t)rc, out, 10);
}

static void run_dmac_overlap_cell(uint32_t api,
                                  const char *api_name, uint32_t direction,
                                  const char *direction_name, uint32_t delta) {
    uint8_t before[sizeof(s_dmac_cell_overlap)];
    uint8_t source_before[DMAC_CELL_BYTES];
    const uint32_t src_start = DMAC_CELL_BASE + (direction ? delta : 0u);
    const uint32_t dst_start = DMAC_CELL_BASE + (direction ? 0u : delta);
    for (uint32_t i = 0; i < sizeof(s_dmac_cell_overlap); ++i) {
        s_dmac_cell_overlap[i] = (uint8_t)(0x47u + (i * 29u));
    }
    memcpy(before, s_dmac_cell_overlap, sizeof(before));
    memcpy(source_before, &before[src_start], sizeof(source_before));
    dmac_cell_cache_before(s_dmac_cell_overlap, sizeof(s_dmac_cell_overlap));

    const int rc = dmac_call(api, &s_dmac_cell_overlap[dst_start],
                             &s_dmac_cell_overlap[src_start], DMAC_CELL_BYTES);
    dmac_cell_cache_after(s_dmac_cell_overlap, sizeof(s_dmac_cell_overlap));

    const uint32_t prefix = dmac_cell_prefix_matches(
        &s_dmac_cell_overlap[dst_start], source_before, DMAC_CELL_BYTES);
    const uint32_t matches = dmac_cell_matches(
        &s_dmac_cell_overlap[dst_start], source_before, DMAC_CELL_BYTES);
    const uint32_t guards = dmac_cell_guard_mutations(
        before, s_dmac_cell_overlap, sizeof(s_dmac_cell_overlap),
        dst_start, DMAC_CELL_BYTES);
    uint32_t out[10] = {
        api, direction, delta, DMAC_CELL_BYTES, prefix, matches, guards,
        dmac_cell_hash(s_dmac_cell_overlap, sizeof(s_dmac_cell_overlap)),
        (uint32_t)(uintptr_t)&s_dmac_cell_overlap[src_start],
        (uint32_t)(uintptr_t)&s_dmac_cell_overlap[dst_start],
    };
    char case_id[48];
    snprintf(case_id, sizeof(case_id), "overlap-%s-%s-%02x",
             api_name, direction_name, (unsigned int)delta);
    defer_record(case_id, guards == 0u ? "PASS" : "FAIL",
                 (uint32_t)rc, out, 10);
}

static void run_dmac_cells(int emulated) {
    const uint32_t apis[] = {DMAC_API_MEMCPY, DMAC_API_TRY_MEMCPY};
    const char *const api_names[] = {"memcpy", "try"};
    const char *const sides[] = {"src", "dst", "both"};
    for (size_t api_i = 0; api_i < sizeof(apis) / sizeof(apis[0]); ++api_i) {
        for (size_t offset_i = 0;
             offset_i < sizeof(s_dmac_cell_offsets) / sizeof(s_dmac_cell_offsets[0]);
             ++offset_i) {
            const uint32_t offset = s_dmac_cell_offsets[offset_i];
            run_dmac_alignment_cell(apis[api_i], api_names[api_i],
                                    sides[0], offset, 0u);
            run_dmac_alignment_cell(apis[api_i], api_names[api_i],
                                    sides[1], 0u, offset);
            run_dmac_alignment_cell(apis[api_i], api_names[api_i],
                                    sides[2], offset, offset);
        }
    }
    for (size_t api_i = 0; api_i < sizeof(apis) / sizeof(apis[0]); ++api_i) {
        for (size_t offset_i = 0;
             offset_i < sizeof(s_dmac_overlap_offsets) / sizeof(s_dmac_overlap_offsets[0]);
             ++offset_i) {
            const uint32_t delta = s_dmac_overlap_offsets[offset_i];
            run_dmac_overlap_cell(apis[api_i], api_names[api_i],
                                  0u, "forward", delta);
            run_dmac_overlap_cell(apis[api_i], api_names[api_i],
                                  1u, "backward", delta);
        }
    }
    const uint32_t done_out[1] = {92u};
    defer_record("dmac-cells-done", "PASS", 0u, done_out, 1);
    flush_deferred(emulated, "PSP-DMAC-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_LATE || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_PRIORITY || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_VBLANK_WINDOW

/* Long-interrupt-mask display accounting.
 *
 * The accepted #88 record established the SHORT-window facts: system time keeps
 * advancing across `sceKernelCpuSuspendIntr`, guest VCOUNT does not, and exactly
 * one VBLANK handler call is taken on resume.  It never sampled VCOUNT
 * immediately after `sceKernelCpuResumeIntr`, so it cannot distinguish
 *
 *   (A) the counter catches up by every period that elapsed under the mask,
 *   (B) exactly one deferred period is credited, or
 *   (C) the elapsed periods are permanently absent from software VCOUNT.
 *
 * These two cases measure that difference directly and never assume a period
 * length: the vblank period is calibrated on the same device in the same run.
 * Every spin is bounded by BOTH an elapsed-system-time test and an iteration
 * cap, so a stopped clock cannot turn a probe into a hang. */

#define MASK_TRIALS      12
#define MASK_SPIN_CAP    40000000u   /* iteration ceiling; never the normal exit */
#define CALIB_FRAMES     60

/* Spin until `want` microseconds of system time have elapsed since `t0`, or the
 * iteration cap trips.  Returns the measured elapsed microseconds.  The volatile
 * sink stops the compiler from discarding the loop. */
static volatile uint32_t s_spin_sink;
#if PSP_ORACLE_CASE != PSP_ORACLE_CASE_DISPLAY_VBLANK_WINDOW
static uint32_t spin_us(uint32_t t0, uint32_t want, uint32_t *iters_out) {
    uint32_t i = 0;
    uint32_t now = t0;
    for (; i < MASK_SPIN_CAP; i++) {
        now = sceKernelGetSystemTimeLow();
        if ((uint32_t)(now - t0) >= want) break;
        s_spin_sink = i;
    }
    if (iters_out) *iters_out = i;
    return (uint32_t)(now - t0);
}
#endif

/* Measure the device's own vblank period without assuming 60000/1001.  Returns
 * nanoseconds per period; 0 if the display never advanced. */
static uint32_t calibrate_period_ns(uint32_t *vc_frames_out) {
    sceDisplayWaitVblankStart();
    uint32_t st0 = sceKernelGetSystemTimeLow();
    uint32_t vc0 = sceDisplayGetVcount();
    for (int i = 0; i < CALIB_FRAMES; i++) sceDisplayWaitVblankStart();
    uint32_t st1 = sceKernelGetSystemTimeLow();
    uint32_t vc1 = sceDisplayGetVcount();
    uint32_t frames = vc1 - vc0;
    if (vc_frames_out) *vc_frames_out = frames;
    if (!frames) return 0;
    return (uint32_t)(((uint64_t)(uint32_t)(st1 - st0) * 1000ull) / frames);
}

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY
static uint32_t periods_in(uint32_t span_us, uint32_t period_ns) {
    if (!period_ns) return 0;
    return (uint32_t)(((uint64_t)span_us * 1000ull) / period_ns);
}
#endif
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT
static const uint32_t k_mask_us[] = { 4000u, 16700u, 30000u, 50000u };
#define MASK_DURATIONS ((int)(sizeof(k_mask_us) / sizeof(k_mask_us[0])))

static void run_display_mask_vcount(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);
    const uint32_t cpu_mhz = (uint32_t)scePowerGetCpuClockFrequencyInt();

    for (int d = 0; d < MASK_DURATIONS; d++) {
        const uint32_t want = k_mask_us[d];
        uint32_t out[29];
        uint32_t trials = 0;
        uint32_t span_min = 0xffffffffu, span_max = 0, span_sum = 0;
        uint32_t per_min = 0xffffffffu, per_max = 0, per_sum = 0;
        uint32_t dur_min = 0xffffffffu, dur_max = 0;              /* vcD - vc0 */
        uint32_t imm_min = 0xffffffffu, imm_max = 0, imm_sum = 0;  /* vcI - vc0 */
        uint32_t n_zero = 0, n_one = 0, n_full = 0, n_partial = 0;
        uint32_t nx1_min = 0xffffffffu, nx1_max = 0, n_next_one = 0;
        uint32_t ahd_min = 0xffffffffu, ahd_max = 0;
        uint32_t ahi_min = 0xffffffffu, ahi_max = 0;
        uint32_t hc_min = 0xffffffffu, hc_max = 0;
        uint32_t n_time_moved = 0;

        for (int k = 0; k < MASK_TRIALS; k++) {
            sceDisplayWaitVblankStart();          /* phase-align to a boundary */
            const uint32_t st0 = sceKernelGetSystemTimeLow();
            const uint32_t vc0 = sceDisplayGetVcount();
            const uint32_t ah0 = (uint32_t)sceDisplayGetAccumulatedHcount();

            const int tok = sceKernelCpuSuspendIntr();
            const uint32_t span = spin_us(st0, want, NULL);
            const uint32_t vcD = sceDisplayGetVcount();
            const uint32_t ahD = (uint32_t)sceDisplayGetAccumulatedHcount();
            const uint32_t hcD = (uint32_t)sceDisplayGetCurrentHcount();
            sceKernelCpuResumeIntr(tok);

            /* The single measurement the accepted record is missing. */
            const uint32_t vcI = sceDisplayGetVcount();
            const uint32_t ahI = (uint32_t)sceDisplayGetAccumulatedHcount();

            sceDisplayWaitVblankStart();
            const uint32_t vcN1 = sceDisplayGetVcount();

            const uint32_t periods = periods_in(span, period_ns);
            const uint32_t d_dur = vcD - vc0;
            const uint32_t d_imm = vcI - vc0;
            const uint32_t d_nx1 = vcN1 - vcI;

            trials++;
            if (span) n_time_moved++;
            if (span < span_min) span_min = span;
            if (span > span_max) span_max = span;
            span_sum += span;
            if (periods < per_min) per_min = periods;
            if (periods > per_max) per_max = periods;
            per_sum += periods;
            if (d_dur < dur_min) dur_min = d_dur;
            if (d_dur > dur_max) dur_max = d_dur;
            if (d_imm < imm_min) imm_min = d_imm;
            if (d_imm > imm_max) imm_max = d_imm;
            imm_sum += d_imm;
            if (d_imm == 0u) n_zero++;
            else if (d_imm == 1u) n_one++;
            if (periods >= 2u && d_imm == periods) n_full++;
            else if (d_imm > 1u && periods >= 2u && d_imm < periods) n_partial++;
            if (d_nx1 < nx1_min) nx1_min = d_nx1;
            if (d_nx1 > nx1_max) nx1_max = d_nx1;
            if (d_nx1 == 1u) n_next_one++;
            {
                const uint32_t dahd = ahD - ah0, dahi = ahI - ah0;
                if (dahd < ahd_min) ahd_min = dahd;
                if (dahd > ahd_max) ahd_max = dahd;
                if (dahi < ahi_min) ahi_min = dahi;
                if (dahi > ahi_max) ahi_max = dahi;
            }
            if (hcD < hc_min) hc_min = hcD;
            if (hcD > hc_max) hc_max = hcD;
        }

        out[0]  = trials;
        out[1]  = want;
        out[2]  = span_min;
        out[3]  = span_max;
        out[4]  = per_min;
        out[5]  = per_max;
        out[6]  = dur_min;
        out[7]  = dur_max;
        out[8]  = imm_min;
        out[9]  = imm_max;
        out[10] = n_zero;
        out[11] = n_one;
        out[12] = n_full;
        out[13] = n_partial;
        out[14] = nx1_min;
        out[15] = nx1_max;
        out[16] = ahd_min;
        out[17] = ahd_max;
        out[18] = ahi_min;
        out[19] = ahi_max;
        out[20] = imm_sum;
        out[21] = per_sum;
        out[22] = trials ? span_sum / trials : 0u;
        out[23] = n_time_moved;
        out[24] = hc_min;
        out[25] = hc_max;
        out[26] = period_ns;
        out[27] = cpu_mhz;
        out[28] = n_next_one;

        {
            char case_id[64];
            snprintf(case_id, sizeof(case_id), "display-mask-vcount-%uus", (unsigned int)want);
            /* PASS states only that every trial ran and the masked window really
               elapsed; it makes no claim about which semantic the numbers show. */
            const int ok = (trials == (uint32_t)MASK_TRIALS) &&
                           (n_time_moved == trials) && (period_ns != 0u) &&
                           (calib_frames >= (uint32_t)(CALIB_FRAMES - 2));
            emit_record_extended(emulated, "PSP-DISPLAY-001", case_id,
                                 ok ? "PASS" : "FAIL", period_ns, out, 29);
        }
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY
/* Sustained duty cycle.  One resume per mask span, many periods per span: the
 * three candidate semantics separate by a factor of ~4 in the accumulated
 * VCOUNT over a fixed wall window, which no single-trial rounding can fake. */
#define DUTY_WINDOW_US 2000000u
#define DUTY_GAP_US       2000u

static const uint32_t k_duty_us[] = { 33000u, 66000u };
#define DUTY_SETTINGS ((int)(sizeof(k_duty_us) / sizeof(k_duty_us[0])))

static void run_display_mask_duty(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);

    for (int d = 0; d < DUTY_SETTINGS; d++) {
        const uint32_t want = k_duty_us[d];
        uint32_t out[13];
        uint32_t episodes = 0, masked_sum = 0, unmasked_sum = 0;
        uint32_t span_min = 0xffffffffu, span_max = 0;

        sceDisplayWaitVblankStart();
        const uint32_t wall0 = sceKernelGetSystemTimeLow();
        const uint32_t vc0 = sceDisplayGetVcount();
        const uint32_t ah0 = (uint32_t)sceDisplayGetAccumulatedHcount();

        while ((uint32_t)(sceKernelGetSystemTimeLow() - wall0) < DUTY_WINDOW_US &&
               episodes < 4096u) {
            const int tok = sceKernelCpuSuspendIntr();
            const uint32_t m0 = sceKernelGetSystemTimeLow();
            const uint32_t span = spin_us(m0, want, NULL);
            sceKernelCpuResumeIntr(tok);
            episodes++;
            masked_sum += span;
            if (span < span_min) span_min = span;
            if (span > span_max) span_max = span;
            /* Unmasked servicing gap: long enough for the deferred delivery and
               USB/PSPLink to run, short enough that it carries few periods. */
            const uint32_t g0 = sceKernelGetSystemTimeLow();
            unmasked_sum += spin_us(g0, DUTY_GAP_US, NULL);
        }

        const uint32_t wall = (uint32_t)(sceKernelGetSystemTimeLow() - wall0);
        const uint32_t vcd = sceDisplayGetVcount() - vc0;
        const uint32_t ahd = (uint32_t)sceDisplayGetAccumulatedHcount() - ah0;
        const uint32_t expected = periods_in(wall, period_ns);

        out[0]  = wall;
        out[1]  = want;
        out[2]  = DUTY_GAP_US;
        out[3]  = episodes;
        out[4]  = vcd;
        out[5]  = expected;
        out[6]  = ahd;
        out[7]  = masked_sum;
        out[8]  = unmasked_sum;
        out[9]  = expected ? (uint32_t)(((uint64_t)vcd * 1000ull) / expected) : 0u;
        out[10] = span_min;
        out[11] = span_max;
        out[12] = period_ns;

        {
            char case_id[64];
            snprintf(case_id, sizeof(case_id), "display-mask-duty-%uus", (unsigned int)want);
            const int ok = episodes > 8u && period_ns != 0u && wall >= DUTY_WINDOW_US;
            emit_record_extended(emulated, "PSP-DISPLAY-001", case_id,
                                 ok ? "PASS" : "FAIL", period_ns, out, 13);
        }
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_BREAK_CONTINUE || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN
/* Bounded GE waits.  A probe never blocks on the GE without a bound: on a
 * PSP-3000 a blocking sceGeListSync(qid, 0) after sceGeBreak/sceGeContinue
 * never returned, and the rest of the case and its completion record were
 * lost.  Every GE wait therefore polls the non-blocking peek (sync type 1)
 * against a deadline on the system timer and returns the last observed
 * states, so a GE that does not drain becomes a measured TIMEOUT outcome.
 *
 * Idle means sceGeDrawSync(1) reports PSP_GE_LIST_DONE: no display list is
 * left to run.  The per-list state is recorded as observed but is not the
 * completion test, because a finished list's queue ID need not stay valid.
 * Each probe list is a few kilobytes of commands that the GE finishes in
 * milliseconds, so half a second is a generous bound. */
#define GE_IDLE_DEADLINE_US 500000u
#define GE_IDLE_POLL_US 100u

typedef struct {
    int list_state;       /* last sceGeListSync(qid, 1); qid itself when qid < 0 */
    int draw_state;       /* last sceGeDrawSync(1)                              */
    uint32_t elapsed_us;  /* system time from the first peek to the last        */
    int idle;             /* draw_state == PSP_GE_LIST_DONE                     */
} GeIdleWait;

/* Peek until the GE is idle or deadline_us has passed; deadline 0 peeks once. */
static GeIdleWait ge_wait_idle(int qid, uint32_t deadline_us) {
    GeIdleWait w;
    const uint32_t start = sceKernelGetSystemTimeLow();
    for (;;) {
        w.list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
        w.draw_state = sceGeDrawSync(1);
        w.elapsed_us = (uint32_t)(sceKernelGetSystemTimeLow() - start);
        w.idle = w.draw_state == PSP_GE_LIST_DONE;
        if (w.idle || w.elapsed_us >= deadline_us) {
            return w;
        }
        sceKernelDelayThread(GE_IDLE_POLL_US);
    }
}

/* Drop every queued list after a wait that did not reach idle, so the next
 * step starts from an empty GE.  sceGeBreak(1, ...) resets all queues
 * (PSPSDK pspge.h); *after receives a bounded wait that shows whether the
 * reset left the GE idle.  Returns the sceGeBreak result. */
static int ge_reset_queues(GeIdleWait *after) {
    PspGeBreakParam param;
    memset(&param, 0, sizeof(param));
    const int rc = sceGeBreak(1, &param);
    *after = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);
    return rc;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_BREAK_CONTINUE

/* Does the PSP GE make forward progress while CPU interrupt delivery is masked?
 *
 * The experiment is stall-gated so that "the GE simply finished before the mask
 * opened" cannot produce a false positive: the list is enqueued already stalled
 * at its first word, the destination is proven untouched, the CPU mask is taken,
 * and only then is the stall released.  Any destination change observed before
 * `sceKernelCpuResumeIntr` is therefore work the GE did with CPU interrupts off.
 *
 * The observable is memory the GE itself writes -- a chain of source-owned block
 * transfers into VRAM tiles -- read back exclusively through the UNCACHED VRAM
 * mirror, so a stale CPU cache line can never be misread as "the GE did not
 * progress".  GE completion notification is measured separately, because the
 * finish handler runs in interrupt context and is expected to stay pending.
 *
 * Nothing here touches retail code or data. */

#define GE_TILES        16
#define GE_TILE_W       512u          /* pixels, 4 bytes each                */
#define GE_TILE_H        32u
#define GE_TILE_BYTES   (GE_TILE_W * GE_TILE_H * 4u)
#define GE_SENTINEL     0xA5A5A5A5u
#define GE_PAYLOAD      0x5A5A5A5Au
#define GE_MASK_CAP_US  40000u        /* bounded masked poll window          */
#define GE_SPIN_CAP     20000000u
#define GE_TRIALS       12

/* Uncached mirrors.  VRAM 0x04000000 is the cached view, 0x44000000 the
   uncached one; main RAM 0x08800000 mirrors at 0x48800000. */
#define UNCACHED(p)  ((volatile uint32_t *)((uintptr_t)(p) | 0x40000000u))

/* GE command words: (cmd << 24) | 24-bit payload. */
#define GE_CMD_NOP            0x00
#define GE_CMD_END            0x0C
#define GE_CMD_FINISH         0x0F
#define GE_CMD_TRANSFERSRC    0xB2
#define GE_CMD_TRANSFERSRCW   0xB3
#define GE_CMD_TRANSFERDST    0xB4
#define GE_CMD_TRANSFERDSTW   0xB5
#define GE_CMD_TRANSFERSTART  0xEA
#define GE_CMD_TRANSFERSRCPOS 0xEB
#define GE_CMD_TRANSFERDSTPOS 0xEC
#define GE_CMD_TRANSFERSIZE   0xEE

static uint32_t s_ge_list[GE_TILES * 10 + 8] __attribute__((aligned(64)));
static uint32_t s_ge_src[GE_TILE_W * GE_TILE_H] __attribute__((aligned(64)));
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK
static volatile int s_ge_finish_calls;
static volatile int s_ge_signal_calls;

static void ge_finish_cb(int id, void *arg) { (void)id; (void)arg; s_ge_finish_calls++; }
static void ge_signal_cb(int id, void *arg) { (void)id; (void)arg; s_ge_signal_calls++; }
#endif

static uint32_t ge_cmd(uint32_t cmd, uint32_t payload) {
    return (cmd << 24) | (payload & 0x00ffffffu);
}

/* Address is split: low 24 bits in the ADDR command, bits 24..31 in bits 16..23
   of the companion W command, which also carries the stride in pixels. */
static void ge_emit_addr(uint32_t **w, uint32_t addr_cmd, uint32_t w_cmd,
                         uint32_t addr, uint32_t stride_px) {
    *(*w)++ = ge_cmd(addr_cmd, addr & 0x00fffff0u);
    *(*w)++ = ge_cmd(w_cmd, ((addr >> 8) & 0x00ff0000u) | (stride_px & 0x7f8u));
}

static uint32_t ge_build_list(uint32_t src, uint32_t dst_base) {
    uint32_t *w = s_ge_list;
    for (int t = 0; t < GE_TILES; t++) {
        ge_emit_addr(&w, GE_CMD_TRANSFERSRC, GE_CMD_TRANSFERSRCW, src, GE_TILE_W);
        ge_emit_addr(&w, GE_CMD_TRANSFERDST, GE_CMD_TRANSFERDSTW,
                     dst_base + (uint32_t)t * GE_TILE_BYTES, GE_TILE_W);
        *w++ = ge_cmd(GE_CMD_TRANSFERSRCPOS, 0u);
        *w++ = ge_cmd(GE_CMD_TRANSFERDSTPOS, 0u);
        *w++ = ge_cmd(GE_CMD_TRANSFERSIZE, ((GE_TILE_H - 1u) << 10) | (GE_TILE_W - 1u));
        *w++ = ge_cmd(GE_CMD_TRANSFERSTART, 1u);   /* bit0 = 4 bytes per pixel */
    }
    *w++ = ge_cmd(GE_CMD_FINISH, 0u);
    *w++ = ge_cmd(GE_CMD_END, 0u);
    *w++ = ge_cmd(GE_CMD_NOP, 0u);
    *w++ = ge_cmd(GE_CMD_NOP, 0u);
    return (uint32_t)((char *)w - (char *)s_ge_list);
}

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK
/* Paint every destination tile with the sentinel through the uncached mirror and
   confirm it reads back, so a failed pre-fill can never look like GE progress. */
static int ge_prime_dst(uint32_t dst_base) {
    for (int t = 0; t < GE_TILES; t++) {
        volatile uint32_t *p = UNCACHED(dst_base + (uint32_t)t * GE_TILE_BYTES);
        p[0] = GE_SENTINEL;
        p[(GE_TILE_BYTES / 4u) - 1u] = GE_SENTINEL;
    }
    for (int t = 0; t < GE_TILES; t++) {
        volatile uint32_t *p = UNCACHED(dst_base + (uint32_t)t * GE_TILE_BYTES);
        if (p[0] != GE_SENTINEL || p[(GE_TILE_BYTES / 4u) - 1u] != GE_SENTINEL) return 0;
    }
    return 1;
}

/* Number of leading tiles whose first AND last word have left the sentinel. */
static int ge_tiles_done(uint32_t dst_base) {
    int n = 0;
    for (int t = 0; t < GE_TILES; t++) {
        volatile uint32_t *p = UNCACHED(dst_base + (uint32_t)t * GE_TILE_BYTES);
        if (p[0] == GE_SENTINEL || p[(GE_TILE_BYTES / 4u) - 1u] == GE_SENTINEL) break;
        n++;
    }
    return n;
}

/* mode 0 = PRIMARY (release the stall while masked)
   mode 1 = CONTROL A (hold the stall while masked -- must stay untouched)
   mode 2 = CONTROL B (release the stall with interrupts enabled -- must change) */
static void ge_run_case(int emulated, int mode, const char *case_id) {
    const uint32_t vram = (uint32_t)(uintptr_t)sceGeEdramGetAddr();
    const uint32_t dst_base = vram + 0x00100000u;      /* top 1 MiB of eDRAM */
    const uint32_t src = (uint32_t)(uintptr_t)s_ge_src;
    const uint32_t list = (uint32_t)(uintptr_t)s_ge_list;

    PspGeCallbackData cb;
    memset(&cb, 0, sizeof(cb));
    cb.signal_func = ge_signal_cb;
    cb.finish_func = ge_finish_cb;
    const int cbid = sceGeSetCallback(&cb);

    for (uint32_t i = 0; i < GE_TILE_W * GE_TILE_H; i++) s_ge_src[i] = GE_PAYLOAD;

    uint32_t out[25];
    memset(out, 0, sizeof(out));
    uint32_t trials = 0, prefill_ok = 0, pre_clean = 0;
    uint32_t masked_any = 0, masked_all = 0, masked_none = 0;
    uint32_t tiles_masked_min = 0xffffffffu, tiles_masked_max = 0;
    uint32_t first_us_min = 0xffffffffu, first_us_max = 0;
    uint32_t all_us_min = 0xffffffffu, all_us_max = 0;
    uint32_t fin_during = 0, fin_after_resume = 0, fin_after_sync = 0;
    uint32_t release_rc_nonzero = 0, enq_bad = 0;
    uint32_t span_min = 0xffffffffu, span_max = 0;
    uint32_t after_resume_all = 0, after_sync_all = 0;
    uint32_t drain_timeouts = 0;

    const uint32_t list_bytes = ge_build_list(src, dst_base);

    for (int k = 0; k < GE_TRIALS; k++) {
        sceKernelDcacheWritebackAll();
        if (!ge_prime_dst(dst_base)) continue;
        prefill_ok++;
        s_ge_finish_calls = 0;
        s_ge_signal_calls = 0;

        /* Enqueue already stalled at the first word: nothing may execute yet. */
        const int qid = sceGeListEnQueue((void *)list, (void *)list, cbid, NULL);
        if (qid < 0) { enq_bad++; continue; }

        /* Step 4 of the design: the destination is still untouched. */
        const int before = ge_tiles_done(dst_base);
        if (before == 0) pre_clean++;

        uint32_t tiles_masked = 0, t_first = 0, t_all = 0, span = 0;
        int fin_masked = 0;

        if (mode == 2) {
            /* CONTROL B: identical release, interrupts left enabled. */
            const uint32_t t0 = sceKernelGetSystemTimeLow();
            const int rc = sceGeListUpdateStallAddr(qid, (void *)(list + list_bytes));
            if (rc < 0) release_rc_nonzero++;
            uint32_t i = 0;
            for (; i < GE_SPIN_CAP; i++) {
                tiles_masked = (uint32_t)ge_tiles_done(dst_base);
                span = (uint32_t)(sceKernelGetSystemTimeLow() - t0);
                if (tiles_masked && !t_first) t_first = span ? span : 1u;
                if (tiles_masked >= (uint32_t)GE_TILES) { t_all = span ? span : 1u; break; }
                if (span >= GE_MASK_CAP_US) break;
            }
            fin_masked = s_ge_finish_calls;
        } else {
            const int tok = sceKernelCpuSuspendIntr();
            const uint32_t t0 = sceKernelGetSystemTimeLow();
            int rc = 0;
            if (mode == 0)
                rc = sceGeListUpdateStallAddr(qid, (void *)(list + list_bytes));
            uint32_t i = 0;
            for (; i < GE_SPIN_CAP; i++) {
                tiles_masked = (uint32_t)ge_tiles_done(dst_base);
                span = (uint32_t)(sceKernelGetSystemTimeLow() - t0);
                if (tiles_masked && !t_first) t_first = span ? span : 1u;
                if (tiles_masked >= (uint32_t)GE_TILES) { t_all = span ? span : 1u; break; }
                if (span >= GE_MASK_CAP_US) break;
            }
            fin_masked = s_ge_finish_calls;   /* sampled while still masked */
            sceKernelCpuResumeIntr(tok);
            if (rc < 0) release_rc_nonzero++;
        }

        if (fin_masked) fin_during++;
        if (ge_tiles_done(dst_base) >= GE_TILES) after_resume_all++;
        if (s_ge_finish_calls) fin_after_resume++;

        /* Drain: CONTROL A never released the stall, so release it now with
           interrupts enabled and wait (bounded) for the GE to go idle.  A
           drain that times out is counted and the queues are reset, leaving
           the GE clean for the next trial either way. */
        if (mode == 1) sceGeListUpdateStallAddr(qid, (void *)(list + list_bytes));
        const GeIdleWait drain = ge_wait_idle(qid, GE_IDLE_DEADLINE_US);
        if (!drain.idle) {
            GeIdleWait after_reset;
            drain_timeouts++;
            (void)ge_reset_queues(&after_reset);
        }
        if (s_ge_finish_calls) fin_after_sync++;
        if (ge_tiles_done(dst_base) >= GE_TILES) after_sync_all++;

        trials++;
        if (mode == 1) {
            /* For CONTROL A the interesting count is how many tiles moved while
               the stall was held: it must be zero. */
            if (tiles_masked) masked_any++; else masked_none++;
        } else {
            if (tiles_masked >= (uint32_t)GE_TILES) masked_all++;
            else if (tiles_masked) masked_any++;
            else masked_none++;
        }
        if (tiles_masked < tiles_masked_min) tiles_masked_min = tiles_masked;
        if (tiles_masked > tiles_masked_max) tiles_masked_max = tiles_masked;
        if (t_first) {
            if (t_first < first_us_min) first_us_min = t_first;
            if (t_first > first_us_max) first_us_max = t_first;
        }
        if (t_all) {
            if (t_all < all_us_min) all_us_min = t_all;
            if (t_all > all_us_max) all_us_max = t_all;
        }
        if (span < span_min) span_min = span;
        if (span > span_max) span_max = span;
    }

    sceGeUnsetCallback(cbid);

    out[0]  = trials;
    out[1]  = (uint32_t)mode;
    out[2]  = prefill_ok;
    out[3]  = pre_clean;
    out[4]  = masked_all;
    out[5]  = masked_any;
    out[6]  = masked_none;
    out[7]  = tiles_masked_min == 0xffffffffu ? 0u : tiles_masked_min;
    out[8]  = tiles_masked_max;
    out[9]  = first_us_min == 0xffffffffu ? 0u : first_us_min;
    out[10] = first_us_max;
    out[11] = all_us_min == 0xffffffffu ? 0u : all_us_min;
    out[12] = all_us_max;
    out[13] = fin_during;
    out[14] = fin_after_resume;
    out[15] = fin_after_sync;
    out[16] = after_resume_all;
    out[17] = after_sync_all;
    out[18] = release_rc_nonzero;
    out[19] = enq_bad;
    out[20] = span_min == 0xffffffffu ? 0u : span_min;
    out[21] = span_max;
    out[22] = (uint32_t)GE_TILES;
    out[23] = (uint32_t)scePowerGetCpuClockFrequencyInt();
    out[24] = drain_timeouts;

    /* PASS means the trial machinery ran and the pre-release destination was
       clean every time.  It asserts nothing about which semantic was observed.
       TIMEOUT means at least one post-trial drain did not reach idle. */
    const int ok = (trials == (uint32_t)GE_TRIALS) &&
                   (prefill_ok == trials) && (pre_clean == trials) && (enq_bad == 0u);
    emit_record_extended(emulated, "PSP-DISPLAY-001", case_id,
                         drain_timeouts ? "TIMEOUT" : (ok ? "PASS" : "FAIL"),
                         (uint32_t)trials, out, 25);
}

static void run_display_ge_mask(int emulated) {
    ge_run_case(emulated, 2, "ge-mask-controlB-enabled-release");
    ge_run_case(emulated, 1, "ge-mask-controlA-stall-held");
    ge_run_case(emulated, 0, "ge-mask-primary-masked-release");
}
#endif
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_TRANSPORT_WRITE
/* Bidirectional host0 file proof: the PSP writes a fixed 64-byte pattern to a
   probe-owned disposable path, reads it back, and reports a checksum plus a
   match flag. The host independently hashes the file it receives. Pattern byte
   i is (0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF. Only this one path is touched. */
#define TRANSPORT_PATH PROBE_HOST0_ROUNDTRIP_PATH
#define TRANSPORT_LEN 64u
static uint32_t transport_fnv1a(const uint8_t *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}
static int run_transport_write_case(int emulated, uint32_t *out) {
    uint8_t pattern[TRANSPORT_LEN];
    uint8_t back[TRANSPORT_LEN];
    for (uint32_t i = 0; i < TRANSPORT_LEN; i++) {
        pattern[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(back, 0, sizeof(back));
    out[0] = transport_fnv1a(pattern, sizeof(pattern));
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint32_t)scePowerGetCpuClockFrequencyInt();
    SceUID fd = sceIoOpen(TRANSPORT_PATH,
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return (int)fd;
    }
    const int written = sceIoWrite(fd, pattern, (SceSize)sizeof(pattern));
    out[1] = (uint32_t)(written < 0 ? 0 : written);
    sceIoClose(fd);
    if (written != (int)sizeof(pattern)) {
        return -1;
    }
    fd = sceIoOpen(TRANSPORT_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return (int)fd;
    }
    const int got = sceIoRead(fd, back, (SceSize)sizeof(back));
    out[2] = (uint32_t)(got < 0 ? 0 : got);
    sceIoClose(fd);
    if (got != (int)sizeof(back)) {
        return -1;
    }
    const uint32_t match =
        (memcmp(pattern, back, sizeof(pattern)) == 0) ? 1u : 0u;
    out[3] = match;
    const int pass = (match == 1u) ? 0 : -1;
    emit_record_extended(emulated, "PSP-TRANSPORT-001",
                         "host0-write-readback", match == 1u ? "PASS" : "FAIL",
                         (uint32_t)pass, out, 5);
    return pass;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_EXIT_DELETE
/* Exit/delete boundary matrix: implicit return vs sceKernelExitThread vs
   sceKernelExitDeleteThread, across positive / zero / small-negative /
   error-shaped statuses. Each cell observes through sceKernelWaitThreadEnd
   AND SceKernelThreadInfo.exitStatus, then probes post-state (delete and
   restart legality). One thread per cell, sequential, immediate exits; NULL
   waits are against threads that exit at once (deterministic sync).
   PASS = harness completed and all raw observations captured; raw API
   outcomes (including errors) are data in result/out*, never converted. */
#define ED_METHOD_RETURN 0u
#define ED_METHOD_EXIT 1u
#define ED_METHOD_EXITDELETE 2u
static uint32_t g_ed_status;
static uint32_t g_ed_method;
static int ed_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (g_ed_method == ED_METHOD_EXIT) {
        sceKernelExitThread((int)g_ed_status);
        return 0x55; /* unreachable */
    }
    if (g_ed_method == ED_METHOD_EXITDELETE) {
        sceKernelExitDeleteThread((int)g_ed_status);
        return 0x55; /* unreachable */
    }
    return (int)g_ed_status;
}
static int run_exit_delete_cell(int emulated, const char *case_id,
                                uint32_t status, uint32_t method) {
    uint32_t out[6] = {0xffffffffu, 0xffffffffu, 0xffffffffu,
                       0xffffffffu, 0xffffffffu, 0xffffffffu};
    g_ed_status = status;
    g_ed_method = method;
    SceUID thid = sceKernelCreateThread(case_id, ed_entry, 32, 0x1000, 0, NULL);
    if (thid < 0) {
        emit_record_extended(emulated, "PSP-THREAD-EXIT-001", case_id, "FAIL",
                             (uint32_t)thid, out, 6);
        return (int)thid;
    }
    out[5] = (uint32_t)sceKernelStartThread(thid, 0, NULL);
    if ((int)out[5] < 0) {
        sceKernelDeleteThread(thid);
        emit_record_extended(emulated, "PSP-THREAD-EXIT-001", case_id, "FAIL",
                             out[5], out, 6);
        return (int)out[5];
    }
    out[0] = (uint32_t)sceKernelWaitThreadEnd(thid, NULL);
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelReferThreadStatus(thid, &info) == 0) {
        out[1] = (uint32_t)info.exitStatus;
        out[2] = (uint32_t)info.status;
    }
    out[3] = (uint32_t)sceKernelDeleteThread(thid);
    out[4] = (uint32_t)sceKernelStartThread(thid, 0, NULL);
    if (method != ED_METHOD_EXITDELETE && (int)out[4] == 0) {
        /* Accepted restart: wait for the rerun, then delete, so the launch
           leaks nothing in-process. */
        sceKernelWaitThreadEnd(thid, NULL);
        sceKernelDeleteThread(thid);
    }
    emit_record_extended(emulated, "PSP-THREAD-EXIT-001", case_id, "PASS",
                         out[0], out, 6);
    return 0;
}
static void run_thread_exit_delete(int emulated) {
    static const struct {
        const char *id;
        uint32_t status;
        uint32_t method;
    } cells[] = {
        {"ED-R77", 0x00000077u, ED_METHOD_RETURN},
        {"ED-R00", 0x00000000u, ED_METHOD_RETURN},
        {"ED-RNEG", 0xffffffefu, ED_METHOD_RETURN},
        {"ED-RERR", 0x800201acu, ED_METHOD_RETURN},
        {"ED-X77", 0x00000077u, ED_METHOD_EXIT},
        {"ED-X00", 0x00000000u, ED_METHOD_EXIT},
        {"ED-XNEG", 0xffffffefu, ED_METHOD_EXIT},
        {"ED-XERR", 0x800201acu, ED_METHOD_EXIT},
        {"ED-D77", 0x00000077u, ED_METHOD_EXITDELETE},
        {"ED-D00", 0x00000000u, ED_METHOD_EXITDELETE},
        {"ED-DNEG", 0xffffffefu, ED_METHOD_EXITDELETE},
        {"ED-DERR", 0x800201acu, ED_METHOD_EXITDELETE},
    };
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        run_exit_delete_cell(emulated, cells[i].id, cells[i].status,
                             cells[i].method);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
/* Allocator-observed user-memory boundary survey: redesign input for the
   invalid-tail precedence cells, whose compiled-in end assumption
   (0x0A000000) the firmware rejects by successfully allocating there.
   Scans partition-2 fixed-address allocatability upward, freeing every
   success immediately. Thread-free, main-thread only; failures are ordinary
   error codes, never faults. Nothing is written through the surveyed
   addresses. Emits highest provable base + first failure. */
#define DMAC_SURVEY_STEPS 4u
static const uint32_t dmac_survey_bases[DMAC_SURVEY_STEPS] = {
    0x0b740000u, 0x0b780000u, 0x0b7c0000u, 0x0b7e0000u,
};
static void run_dmac_survey(int emulated) {
    uint32_t top_ok = 0;
    uint32_t first_fail = 0;
    uint32_t first_err = 0;
    uint32_t attempts = 0;
    for (uint32_t i = 0; i < DMAC_SURVEY_STEPS; i++) {
        const uint32_t base = dmac_survey_bases[i];
        attempts++;
        const SceUID got = sceKernelAllocPartitionMemory(
            2, "oracle-dmac-survey", PSP_SMEM_Addr, 0x100,
            (void *)(uintptr_t)base);
        if (got < 0) {
            if (first_fail == 0u) {
                first_fail = base;
                first_err = (uint32_t)got;
            }
            continue;
        }
        if (base > top_ok) {
            top_ok = base;
        }
        sceKernelFreePartitionMemory(got);
    }
    const uint32_t out[] = {
        top_ok, first_fail, first_err, attempts, DMAC_SURVEY_STEPS,
    };
    emit_record_extended(emulated, "PSP-DMAC-001", "allocator-survey", "PASS",
                         top_ok, out, sizeof(out) / sizeof(out[0]));
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CTRL_CLOCK
/* Controller timestamp + clock-domain correlations (main thread only, no
   threads created). Timestamp units/epoch are NOT assumed: every cell pairs
   the controller timestamp against sceKernelGetSystemTimeLow so the host
   derives offset/rate empirically. All deltas are raw u32 wraps (host
   interprets). 8-iteration summaries use min/max only (no float). */
#define CC_ITERS 8
static void run_ctrl_clock(int emulated) {
    SceCtrlData pad;
    /* CC-TS-PAIRS: peek timestamp vs system time, back-to-back. */
    {
        uint32_t ts_prev = 0, sys_prev = 0, ts0 = 0, sys0 = 0;
        uint32_t min_dts = 0xffffffffu, max_dts = 0;
        uint32_t min_dsys = 0xffffffffu, max_dsys = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < CC_ITERS; i++) {
            memset(&pad, 0, sizeof(pad));
            if (sceCtrlPeekBufferPositive(&pad, 1) < 0) {
                continue;
            }
            const uint32_t sys = sceKernelGetSystemTimeLow();
            if (n == 0) {
                ts0 = pad.TimeStamp;
                sys0 = sys;
            } else {
                /* wrap-safe forward deltas via unsigned arithmetic */
                const uint32_t fwd_ts = pad.TimeStamp - ts_prev;
                const uint32_t fwd_sys = sys - sys_prev;
                if (fwd_ts < min_dts) {
                    min_dts = fwd_ts;
                }
                if (fwd_ts > max_dts) {
                    max_dts = fwd_ts;
                }
                if (fwd_sys < min_dsys) {
                    min_dsys = fwd_sys;
                }
                if (fwd_sys > max_dsys) {
                    max_dsys = fwd_sys;
                }
            }
            ts_prev = pad.TimeStamp;
            sys_prev = sys;
            n++;
        }
        const uint32_t out[] = {n, min_dts, max_dts, min_dsys, max_dsys, ts0,
                                sys0};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-ts-pairs",
                             n > 1 ? "PASS" : "FAIL", n, out, 7);
    }
    /* CC-TS-VCOUNT: vcount vs peek timestamp. */
    {
        uint32_t v_prev = 0, ts_prev = 0, v0 = 0, ts00 = 0;
        uint32_t min_dv = 0xffffffffu, max_dv = 0;
        uint32_t min_dts = 0xffffffffu, max_dts = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < CC_ITERS; i++) {
            const uint32_t v = sceDisplayGetVcount();
            memset(&pad, 0, sizeof(pad));
            if (sceCtrlPeekBufferPositive(&pad, 1) < 0) {
                continue;
            }
            if (n == 0) {
                v0 = v;
                ts00 = pad.TimeStamp;
            } else {
                const uint32_t dv = v - v_prev;
                const uint32_t dts = pad.TimeStamp - ts_prev;
                if (dv < min_dv) {
                    min_dv = dv;
                }
                if (dv > max_dv) {
                    max_dv = dv;
                }
                if (dts < min_dts) {
                    min_dts = dts;
                }
                if (dts > max_dts) {
                    max_dts = dts;
                }
            }
            v_prev = v;
            ts_prev = pad.TimeStamp;
            n++;
        }
        const uint32_t out[] = {n, min_dv, max_dv, min_dts, max_dts, v0, ts00};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-ts-vcount",
                             n > 1 ? "PASS" : "FAIL", n, out, 7);
    }
    /* CC-SNAPSHOT: one clock-domain anchor row. */
    {
        const uint32_t sys = sceKernelGetSystemTimeLow();
        const uint32_t v = sceDisplayGetVcount();
        const uint32_t h = sceDisplayGetAccumulatedHcount();
        uint64_t tick = 0;
        const int rtc_rc = sceRtcGetCurrentTick(&tick);
        const uint32_t mhz = (uint32_t)scePowerGetCpuClockFrequencyInt();
        const uint32_t out[] = {sys, v, h, (uint32_t)(tick & 0xffffffffu),
                                (uint32_t)(tick >> 32), mhz,
                                (uint32_t)rtc_rc};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "clock-snapshot",
                             rtc_rc == 0 ? "PASS" : "FAIL", sys, out, 7);
    }
    /* CC-DELAY: 10 ms DelayThread ground truth. */
    {
        const uint32_t before = sceKernelGetSystemTimeLow();
        sceKernelDelayThread(10000);
        const uint32_t after = sceKernelGetSystemTimeLow();
        const uint32_t out[] = {before, after, after - before, 10000u};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "delay-10ms",
                             "PASS", after - before, out, 4);
    }
    /* CC-ZERO: zero-count read behavior. */
    {
        memset(&pad, 0, sizeof(pad));
        const int rc = sceCtrlReadBufferPositive(&pad, 0);
        const uint32_t out[] = {(uint32_t)pad.TimeStamp};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-zero-count",
                             "PASS", (uint32_t)rc, out, 1);
    }
    /* CC-PEEK-READ: does a consuming read change the timestamp? */
    {
        SceCtrlData a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        const int prc = sceCtrlPeekBufferPositive(&a, 1);
        const int rrc = sceCtrlReadBufferPositive(&b, 1);
        const uint32_t out[] = {(uint32_t)a.TimeStamp, (uint32_t)b.TimeStamp,
                                (uint32_t)prc, (uint32_t)rrc};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-peek-read",
                             (prc >= 0 && rrc >= 0) ? "PASS" : "FAIL",
                             (uint32_t)(b.TimeStamp - a.TimeStamp), out, 4);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
/* COP1/FPU result vectors (main thread only, no threads created). Two
   evidence classes, kept distinct in the report: PINNED cells use inline
   asm for the exact opcode (cvt.w.s/cvt.s.w, FCR31 access); COMPILER cells
   use C operators (the compiler selects the sequence — measured as run,
   opcode not pinned). FCR31 exception enables stay OFF throughout (a trap
   would kill the launch); only RM and FS bits are touched, saved/restored
   per cell. All bit patterns travel as u32 (memcpy-punned, volatile).

   MAXIMALLY HARDENED v3:
   1. Boot FCR31 is captured via explicit cfc1 at the entry of main() and
      recorded as diagnostic data, but NEVER restored during execution
      (the boot FCR31 has IEEE trap enables active, 0x0E00).
   2. FCR31 is cleared to 0 (all traps off, flags 0, RM=RN) at the literal
      first instruction of main() and between every single test cell.
   4. Cell index is maintained in volatile memory diagnostic marker
      g_fpu_cell_index before each cell. The unsafe undeclared $s2 register write
      (which clobbered GCC's emulated local variable in v4) is removed;
      the volatile memory store guarantees strict sequence ordering and zero
      register allocation hazards.
      host0:/fpu_vector_log.txt.
   6. Clean ExitGame only once, zero threads created, 16 expected records.
*/
static volatile uint32_t g_fpu_cell_index = 0;

static inline uint32_t fpu_get_fcr31(void) {
    uint32_t v;
    __asm__ volatile("cfc1 %0, $31" : "=r"(v) :: "memory");
    return v;
}

static inline void fpu_set_fcr31(uint32_t v) {
    __asm__ volatile("ctc1 %0, $31" :: "r"(v) : "memory");
}

/* Traps OFF, RM=RN: the only FCR31 state FP arithmetic may run under here.
   The boot FCR31 is never trusted (it carries exception enables 0x0E00). */
static inline void fpu_quiet(void) {
    fpu_set_fcr31(0);
}

/* PINNED: cvt.w.s honors RM. */
static inline int32_t fpu_cvt_w_s(float f) {
    float w;
    __asm__ volatile("cvt.w.s %0, %1" : "=f"(w) : "f"(f) : "memory");
    int32_t r;
    memcpy(&r, &w, 4);
    return r;
}

/* PINNED: cvt.s.w int->float. */
static inline float fpu_cvt_s_w(int32_t i) {
    float in;
    memcpy(&in, &i, 4);
    float f;
    __asm__ volatile("cvt.s.w %0, %1" : "=f"(f) : "f"(in) : "memory");
    return f;
}

static inline uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float u32_f32(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void run_fpu_vector(int emulated, uint32_t boot_fcr31) {
    static const uint32_t inputs[12] = {
        0x3fc00000u, /* 1.5 */
        0x40200000u, /* 2.5 */
        0xbfc00000u, /* -1.5 */
        0xc0200000u, /* -2.5 */
        0x3dccccddu, /* 0.1 */
        0x501502f9u, /* 1e10 */
        0xd01502f9u, /* -1e10 */
        0x7fc00001u, /* quiet NaN, payload 1 */
        0x7f800000u, /* +Inf */
        0xff800000u, /* -Inf */
        0x00000000u, /* +0 */
        0x80000000u, /* -0 */
    };

    /* Cell 0: Diagnostic record of inherited boot FCR31. */
    g_fpu_cell_index = 0;
    {
        const uint32_t out[] = {boot_fcr31};
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-boot-fcr31", "PASS", boot_fcr31, out, 1);
    }

    /* PINNED cells 1..4: cvt.w.s under each RM (0 RN, 1 RZ, 2 RP, 3 RM). */
    for (uint32_t rm = 0; rm < 4; rm++) {
        g_fpu_cell_index = 1 + rm;
        fpu_set_fcr31(rm & 3u); /* RM set, all trap enables strictly 0 */
        uint32_t out[12];
        for (uint32_t i = 0; i < 12; i++) {
            out[i] = (uint32_t)fpu_cvt_w_s(u32_f32(inputs[i]));
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet(); /* clear flags, traps remain 0; NEVER restore boot_fcr31 */
        char cid[32];
        snprintf(cid, sizeof(cid), "fpu-cvt-rm%u", (unsigned int)rm);
        emit_record_extended(emulated, "PSP-FPU-001", cid, "PASS", flags, out, 12);
    }

    /* COMPILER cell 5: C-cast float->int (compiler selects trunc sequence). */
    g_fpu_cell_index = 5;
    {
        uint32_t out[12];
        fpu_quiet();
        for (uint32_t i = 0; i < 12; i++) {
            volatile float vf = u32_f32(inputs[i]);
            out[i] = (uint32_t)(int32_t)vf;
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-ccast-trunc", "PASS", flags, out, 12);
    }

    /* PINNED cell 6: int->float exactness. */
    g_fpu_cell_index = 6;
    {
        static const int32_t ints[6] = {0, 1, -1, 0x7fffffff, (int32_t)0x80000000,
                                        123456789};
        uint32_t out[6];
        fpu_quiet();
        for (uint32_t i = 0; i < 6; i++) {
            out[i] = f32_bits(fpu_cvt_s_w(ints[i]));
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-cvt-s-w", "PASS", flags, out, 6);
    }

    /* COMPILER cells 7..11: flag/edge arithmetic (volatile C operators). */
    {
        struct {
            const char *id;
            float a;
            float b;
            uint32_t op; /* 0=mul 1=div 2=add */
        } const ops[] = {
            {"fpu-flag-overflow", 1e30f, 1e30f, 0},
            {"fpu-flag-div0", 1.0f, 0.0f, 1},
            {"fpu-flag-invalid", 0.0f, 0.0f, 1},
            {"fpu-flag-underflow", 1e-30f, 1e-30f, 0},
            {"fpu-flag-inexact", 0.1f, 0.2f, 2},
        };
        for (uint32_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
            g_fpu_cell_index = 7 + k;
            fpu_quiet();
            volatile float va = ops[k].a;
            volatile float vb = ops[k].b;
            volatile float vr = 0;
            if (ops[k].op == 0u) {
                vr = va * vb;
            } else if (ops[k].op == 1u) {
                vr = va / vb;
            } else {
                vr = va + vb;
            }
            const uint32_t bits = f32_bits(vr);
            const uint32_t flags = fpu_get_fcr31();
            fpu_quiet(); /* clear flags; NEVER restore boot_fcr31 */
            const uint32_t out[] = {bits, flags};
            emit_record_extended(emulated, "PSP-FPU-001", ops[k].id, "PASS", flags, out, 2);
        }
    }

    /* COMPILER cell 12: FTZ contrast (FS=1 vs FS=0) on the underflow op. */
    g_fpu_cell_index = 12;
    {
        volatile float va = 1e-30f;
        volatile float vb = 1e-30f;
        fpu_quiet();
        volatile float r0 = va * vb;
        const uint32_t b0 = f32_bits(r0);
        const uint32_t f0 = fpu_get_fcr31();
        fpu_set_fcr31(0x01000000u); /* FS=1, all enables strictly 0 */
        volatile float r1 = va * vb;
        const uint32_t b1 = f32_bits(r1);
        const uint32_t f1 = fpu_get_fcr31();
        fpu_quiet(); /* clear flags and FS */
        const uint32_t out[] = {b0, f0, b1, f1};
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-ftz-contrast", "PASS", b0 ^ b1, out, 4);
    }

    /* COMPILER cell 13: signed-zero behaviors. */
    g_fpu_cell_index = 13;
    {
        volatile float pz = 0.0f;
        volatile float nz = u32_f32(0x80000000u);
        volatile float inf = u32_f32(0x7f800000u);
        fpu_quiet();
        const uint32_t out[] = {
            f32_bits(pz + nz), f32_bits(nz + nz), f32_bits(1.0f / pz),
            f32_bits(1.0f / nz), f32_bits(pz * inf), f32_bits(nz * inf),
        };
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-signed-zero", "PASS", flags, out, 6);
    }

    /* COMPILER cell 14: NaN payload propagation through + and *. */
    g_fpu_cell_index = 14;
    {
        static const uint32_t nans[3] = {0x7fc00001u, 0x7fffffffu, 0xffc00001u};
        uint32_t out[6];
        fpu_quiet();
        for (uint32_t i = 0; i < 3; i++) {
            volatile float vn = u32_f32(nans[i]);
            volatile float vo = 1.0f;
            out[2 * i] = f32_bits(vn + vo);
            out[2 * i + 1] = f32_bits(vn * vo);
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-nan-payload", "PASS", flags, out, 6);
    }

    /* Cell 15: Done record confirming all preceding cells executed without trap. */
    g_fpu_cell_index = 15;
    fpu_quiet();
    {
        const uint32_t out[] = {15};
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-done", "PASS", 0, out, 1);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE
/* VFPU compare results (main thread only, no threads created). vscmp.s, vsge.s
   and vslt.s each write one lane: S000 and S001 hold the operands and S002 the
   result. Operands and results move as raw u32 words (mtv/mfv), so every record
   carries exact bit patterns and no libc float formatting. Each cell loads a
   sentinel into S002 first, so a compare that did not write its destination
   cannot pass as a measured word.

   The expected words are the project's model, not console behaviour. They
   follow src/rt/vfpu_interp.c (VFPU3 sub-ops 5, 6 and 7, PPSSPP's Int_Vscmp,
   Int_Vsge and Int_Vslt): vscmp.s yields -1.0, 0.0 or +1.0, vsge.s and vslt.s
   yield 1.0 or 0.0, and a NaN operand compares false and yields 0.0. The
   console's NaN results are UNMEASURED. Every record carries result = the
   observed word, out0 = the model word, and out1/out2 = the operands; whether
   the console agrees with the model is result against out0. The status does
   not judge that, as in the fpu-vector family: the campaign runner accepts a
   capture only when every record is PASS, so a disagreement must not fail the
   record that measured it. PASS means the compare wrote S002 (the sentinel is
   gone); FAIL means S002 still held the sentinel, so the cell measured
   nothing.

   FCR31 is cleared to traps-off before the first cell, so no IEEE trap enable
   from the boot state can fire on a signalling NaN; the boot value is never
   restored. Results are computed into a table and emitted after the last cell,
   so no host0 I/O runs between VFPU operations. */
#define VFPU_COMPARE_SENTINEL 0x7a5a5a5au
#define VFPU_COMPARE_OPS 3u
#define VFPU_COMPARE_PAIRS 15u
#define VFPU_COMPARE_CELLS (VFPU_COMPARE_OPS * VFPU_COMPARE_PAIRS)
#define VFPU_COMPARE_ONE 0x3f800000u
#define VFPU_COMPARE_MINUS_ONE 0xbf800000u

struct vfpu_compare_pair {
    const char *name;
    uint32_t a;
    uint32_t b;
    uint32_t expect[VFPU_COMPARE_OPS]; /* model word per op: vscmp.s, vsge.s, vslt.s */
};

static const struct vfpu_compare_pair s_vfpu_compare_pairs[VFPU_COMPARE_PAIRS] = {
    {"lt", 0x3fc00000u, 0x40200000u, {VFPU_COMPARE_MINUS_ONE, 0u, VFPU_COMPARE_ONE}},
    {"eq", 0x3fc00000u, 0x3fc00000u, {0u, VFPU_COMPARE_ONE, 0u}},
    {"gt", 0x40200000u, 0x3fc00000u, {VFPU_COMPARE_ONE, VFPU_COMPARE_ONE, 0u}},
    {"zero-pos-neg", 0x00000000u, 0x80000000u, {0u, VFPU_COMPARE_ONE, 0u}},
    {"zero-neg-pos", 0x80000000u, 0x00000000u, {0u, VFPU_COMPARE_ONE, 0u}},
    {"nan-left-quiet", 0x7fc00000u, 0x3fc00000u, {0u, 0u, 0u}},
    {"nan-right-quiet", 0x3fc00000u, 0x7fc00000u, {0u, 0u, 0u}},
    {"nan-left-signal", 0x7f800001u, 0x3fc00000u, {0u, 0u, 0u}},
    {"nan-right-signal", 0x3fc00000u, 0x7f800001u, {0u, 0u, 0u}},
    {"nan-left-negative", 0xffc00000u, 0x3fc00000u, {0u, 0u, 0u}},
    {"inf-pos-neg", 0x7f800000u, 0xff800000u, {VFPU_COMPARE_ONE, VFPU_COMPARE_ONE, 0u}},
    {"inf-neg-pos", 0xff800000u, 0x7f800000u, {VFPU_COMPARE_MINUS_ONE, 0u, VFPU_COMPARE_ONE}},
    {"inf-pos-pos", 0x7f800000u, 0x7f800000u, {0u, VFPU_COMPARE_ONE, 0u}},
    {"inf-neg-neg", 0xff800000u, 0xff800000u, {0u, VFPU_COMPARE_ONE, 0u}},
    {"inf-pos-finite", 0x7f800000u, 0x3fc00000u, {VFPU_COMPARE_ONE, VFPU_COMPARE_ONE, 0u}},
};

static uint32_t vfpu_compare_vscmp(uint32_t a, uint32_t b) {
    const uint32_t sentinel = VFPU_COMPARE_SENTINEL;
    uint32_t result;
    __asm__ volatile(
        "mtv %2, S002\n"
        "mtv %1, S000\n"
        "mtv %3, S001\n"
        "vscmp.s S002, S000, S001\n"
        "mfv %0, S002\n"
        : "=r"(result) : "r"(a), "r"(sentinel), "r"(b) : "memory");
    return result;
}

static uint32_t vfpu_compare_vsge(uint32_t a, uint32_t b) {
    const uint32_t sentinel = VFPU_COMPARE_SENTINEL;
    uint32_t result;
    __asm__ volatile(
        "mtv %2, S002\n"
        "mtv %1, S000\n"
        "mtv %3, S001\n"
        "vsge.s S002, S000, S001\n"
        "mfv %0, S002\n"
        : "=r"(result) : "r"(a), "r"(sentinel), "r"(b) : "memory");
    return result;
}

static uint32_t vfpu_compare_vslt(uint32_t a, uint32_t b) {
    const uint32_t sentinel = VFPU_COMPARE_SENTINEL;
    uint32_t result;
    __asm__ volatile(
        "mtv %2, S002\n"
        "mtv %1, S000\n"
        "mtv %3, S001\n"
        "vslt.s S002, S000, S001\n"
        "mfv %0, S002\n"
        : "=r"(result) : "r"(a), "r"(sentinel), "r"(b) : "memory");
    return result;
}

static void run_vfpu_compare(int emulated) {
    static const char *const op_names[VFPU_COMPARE_OPS] = {"vscmp", "vsge", "vslt"};
    uint32_t observed[VFPU_COMPARE_CELLS];

    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    probe_step(emulated, "vfpu-compare", "cells");
    for (uint32_t op = 0; op < VFPU_COMPARE_OPS; op++) {
        for (uint32_t p = 0; p < VFPU_COMPARE_PAIRS; p++) {
            const struct vfpu_compare_pair *pair = &s_vfpu_compare_pairs[p];
            uint32_t word;
            if (op == 0) {
                word = vfpu_compare_vscmp(pair->a, pair->b);
            } else if (op == 1) {
                word = vfpu_compare_vsge(pair->a, pair->b);
            } else {
                word = vfpu_compare_vslt(pair->a, pair->b);
            }
            observed[op * VFPU_COMPARE_PAIRS + p] = word;
        }
    }
    for (uint32_t op = 0; op < VFPU_COMPARE_OPS; op++) {
        for (uint32_t p = 0; p < VFPU_COMPARE_PAIRS; p++) {
            const struct vfpu_compare_pair *pair = &s_vfpu_compare_pairs[p];
            const uint32_t index = op * VFPU_COMPARE_PAIRS + p;
            const uint32_t expect = pair->expect[op];
            const uint32_t out[] = {expect, pair->a, pair->b};
            char case_id[40];
            snprintf(case_id, sizeof(case_id), "%s-%s", op_names[op], pair->name);
            emit_record_extended(emulated, "PSP-VFPU-CMP-001", case_id,
                                 observed[index] == VFPU_COMPARE_SENTINEL ? "FAIL" : "PASS",
                                 observed[index], out, 3);
        }
    }
    uint32_t done = VFPU_COMPARE_CELLS;
    emit_record_extended(emulated, "PSP-VFPU-CMP-001", "vfpu-compare-done",
                         "PASS", 0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_TEARDOWN_TEST
/* Retired diagnostic: self-deleting main bypassed the CRT stop handshake.
   Keep the case name buildable while reporting the unsupported experiment. */
static void run_teardown_test(int emulated) {
    const uint32_t self = (uint32_t)sceKernelGetThreadId();
    const uint32_t out[] = {self};
    emit_record_extended(emulated, "PSP-TEARDOWN-001", "exitdelete-main",
                         "SKIP", self, out, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
/* 6 measurement cells + 1 completion sentinel. */
_Static_assert(DEFERRED_MAX_RECORDS >= 7, "io-matrix needs 7 deferred slots");

static void run_io_matrix(int emulated) {
    const char *test_path = "host0:/test_io_matrix.tmp";
    uint32_t out[6];

    /* Cell 1: io-open-create */
    memset(out, 0xFF, sizeof(out));
    SceUID fd = sceIoOpen(test_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    out[0] = (uint32_t)fd;
    defer_record("io-open-create",
                 fd >= 0 ? "PASS" : "FAIL", (uint32_t)fd, out, 1);

    if (fd >= 0) {
        /* Cell 2: io-write */
        memset(out, 0xFF, sizeof(out));
        uint8_t wbuf[64];
        memset(wbuf, 0x5A, sizeof(wbuf));
        int written = sceIoWrite(fd, wbuf, sizeof(wbuf));
        int close_rc = sceIoClose(fd);
        out[0] = (uint32_t)written;
        out[1] = (uint32_t)close_rc;
        defer_record("io-write",
                     (written == 64 && close_rc == 0) ? "PASS" : "FAIL",
                     (uint32_t)written, out, 2);
    }

    /* Cell 3: io-read-verify */
    memset(out, 0xFF, sizeof(out));
    fd = sceIoOpen(test_path, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        uint8_t rbuf[64];
        memset(rbuf, 0, sizeof(rbuf));
        int nread = sceIoRead(fd, rbuf, sizeof(rbuf));
        int match = 1;
        for (int i = 0; i < 64; i++) {
            if (rbuf[i] != 0x5A) { match = 0; break; }
        }
        out[0] = (uint32_t)nread;
        out[1] = (uint32_t)match;
        defer_record("io-read-verify",
                     (nread == 64 && match) ? "PASS" : "FAIL",
                     (uint32_t)nread, out, 2);

        /* Cell 4: io-lseek */
        memset(out, 0xFF, sizeof(out));
        SceOff s_set = sceIoLseek(fd, 32, PSP_SEEK_SET);
        SceOff s_cur = sceIoLseek(fd, -16, PSP_SEEK_CUR);
        SceOff s_end = sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoClose(fd);
        out[0] = (uint32_t)s_set;
        out[1] = (uint32_t)s_cur;
        out[2] = (uint32_t)s_end;
        defer_record("io-lseek",
                     (s_set == 32 && s_cur == 16 && s_end == 64) ? "PASS" : "FAIL",
                     (uint32_t)s_end, out, 3);
    } else {
        out[0] = (uint32_t)fd;
        defer_record("io-read-verify", "FAIL", (uint32_t)fd, out, 1);
    }

    /* Cell 5: io-append */
    memset(out, 0xFF, sizeof(out));
    fd = sceIoOpen(test_path, PSP_O_WRONLY | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        uint8_t abuf[32];
        memset(abuf, 0xA5, sizeof(abuf));
        int app_written = sceIoWrite(fd, abuf, sizeof(abuf));
        sceIoClose(fd);

        fd = sceIoOpen(test_path, PSP_O_RDONLY, 0777);
        SceOff total_sz = sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoClose(fd);
        out[0] = (uint32_t)app_written;
        out[1] = (uint32_t)total_sz;
        defer_record("io-append",
                     (app_written == 32 && total_sz == 96) ? "PASS" : "FAIL",
                     (uint32_t)total_sz, out, 2);
    } else {
        out[0] = (uint32_t)fd;
        defer_record("io-append", "FAIL", (uint32_t)fd, out, 1);
    }

    /* Cell 6: io-errors & cleanup */
    memset(out, 0xFF, sizeof(out));
    SceUID bad_open = sceIoOpen("host0:/__nonexistent_file_matrix_xyz__.tmp", PSP_O_RDONLY, 0777);
    int bad_read = sceIoRead(-1, out, 4);
    int rem_rc = sceIoRemove(test_path);
    out[0] = (uint32_t)bad_open;
    out[1] = (uint32_t)bad_read;
    out[2] = (uint32_t)rem_rc;
    defer_record("io-errors",
                 (bad_open < 0 && bad_read < 0 && rem_rc == 0) ? "PASS" : "FAIL",
                 (uint32_t)bad_open, out, 3);

    /* Every IoFileMgr measurement is now complete.  The completion sentinel
       carries the number of semantic records actually captured, so a truncated
       transport is detectable against the emitted stream length. */
    const uint32_t done_out[1] = {(uint32_t)s_deferred_count};
    defer_record("io-done", "PASS", 0, done_out, 1);

    flush_deferred(emulated, "PSP-IO-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
#define AUDIO_QUERY_SAMPLES 512
#define AUDIO_QUERY_BLOCKS 2
#define AUDIO_NOT_CAPTURED 0xffffffffu

static int16_t s_audio_silence[AUDIO_QUERY_SAMPLES * 2]
    __attribute__((aligned(64)));

static void audio_defer(int emulated, const char *case_id, const char *status,
                        uint32_t result, const uint32_t *out, size_t out_count) {
    (void)emulated;
    defer_record(case_id, status, result, out, out_count);
}

static void run_audio_query(int emulated) {
    uint32_t out[8];
    memset(s_audio_silence, 0, sizeof(s_audio_silence));

    const int channel = sceAudioChReserve(
        0, AUDIO_QUERY_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
    out[0] = (uint32_t)channel;
    out[1] = AUDIO_QUERY_SAMPLES;
    out[2] = PSP_AUDIO_FORMAT_STEREO;
    audio_defer(emulated, "audio-ch-reserve", channel < 0 ? "FAIL" : "PASS",
                channel, out, 3);

    const int rest0_before = channel < 0 ? (int)AUDIO_NOT_CAPTURED
        : sceAudioGetChannelRestLen(channel);
    const int rest1_before = channel < 0 ? (int)AUDIO_NOT_CAPTURED
        : sceAudioGetChannelRestLength(channel);
    out[0] = (uint32_t)rest0_before;
    out[1] = (uint32_t)rest1_before;
    audio_defer(emulated, "audio-ch-query-before", channel < 0 ? "SKIP" : "PASS",
                0, out, 2);

    for (uint32_t block = 0; block < AUDIO_QUERY_BLOCKS; ++block) {
        char case_id[48];
        snprintf(case_id, sizeof(case_id), "audio-ch-output-blocking-%u",
                 (unsigned int)block);
        if (channel < 0) {
            for (size_t i = 0; i < 7; ++i) out[i] = AUDIO_NOT_CAPTURED;
            audio_defer(emulated, case_id, "SKIP", AUDIO_NOT_CAPTURED, out, 7);
            continue;
        }
        const int rest0_pre = sceAudioGetChannelRestLen(channel);
        const int rest1_pre = sceAudioGetChannelRestLength(channel);
        const uint32_t start = sceKernelGetSystemTimeLow();
        const int rc = sceAudioOutputBlocking(
            channel, PSP_AUDIO_VOLUME_MAX, s_audio_silence);
        const uint32_t elapsed = (uint32_t)(sceKernelGetSystemTimeLow() - start);
        const int rest0_post = sceAudioGetChannelRestLen(channel);
        const int rest1_post = sceAudioGetChannelRestLength(channel);
        out[0] = elapsed;
        out[1] = (uint32_t)rest0_pre;
        out[2] = (uint32_t)rest1_pre;
        out[3] = (uint32_t)rest0_post;
        out[4] = (uint32_t)rest1_post;
        out[5] = AUDIO_QUERY_SAMPLES;
        out[6] = (uint32_t)channel;
        audio_defer(emulated, case_id, "PASS", rc, out, 7);
    }

    const uint32_t rest0_after = channel < 0 ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioGetChannelRestLen(channel);
    const uint32_t rest1_after = channel < 0 ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioGetChannelRestLength(channel);
    out[0] = rest0_after;
    out[1] = rest1_after;
    audio_defer(emulated, "audio-ch-query-after", channel < 0 ? "SKIP" : "PASS",
                0, out, 2);

    const uint32_t channel_release = channel < 0 ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioChRelease(channel);
    const int released = channel >= 0 && (int32_t)channel_release >= 0;
    out[0] = channel_release;
    audio_defer(emulated, "audio-ch-release",
                channel < 0 ? "SKIP" : released ? "PASS" : "FAIL",
                channel_release, out, 1);

    /* The just-released channel: what a query on it returns after release. A
     * refused release leaves the channel live, so nothing is measured then. */
    out[0] = !released ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioGetChannelRestLen(channel);
    out[1] = !released ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioGetChannelRestLength(channel);
    audio_defer(emulated, "audio-ch-query-released", released ? "PASS" : "SKIP",
                0, out, 2);

    const int out2 = sceAudioOutput2Reserve(AUDIO_QUERY_SAMPLES);
    out[0] = (uint32_t)out2;
    out[1] = AUDIO_QUERY_SAMPLES;
    audio_defer(emulated, "audio-out2-reserve", out2 < 0 ? "FAIL" : "PASS",
                out2, out, 2);

    const int out2_rest_before = sceAudioOutput2GetRestSample();
    out[0] = (uint32_t)out2_rest_before;
    audio_defer(emulated, "audio-out2-query-before", "PASS", 0, out, 1);

    if (out2 < 0) {
        for (size_t i = 0; i < 4; ++i) out[i] = AUDIO_NOT_CAPTURED;
        audio_defer(emulated, "audio-out2-output-blocking", "SKIP",
                    AUDIO_NOT_CAPTURED, out, 4);
    } else {
        const uint32_t start = sceKernelGetSystemTimeLow();
        const int rc = sceAudioOutput2OutputBlocking(
            PSP_AUDIO_VOLUME_MAX, s_audio_silence);
        const uint32_t elapsed = (uint32_t)(sceKernelGetSystemTimeLow() - start);
        out[0] = elapsed;
        out[1] = (uint32_t)out2_rest_before;
        out[2] = (uint32_t)sceAudioOutput2GetRestSample();
        out[3] = AUDIO_QUERY_SAMPLES;
        audio_defer(emulated, "audio-out2-output-blocking", "PASS", rc, out, 4);
    }

    out[0] = (uint32_t)sceAudioOutput2GetRestSample();
    audio_defer(emulated, "audio-out2-query-after", out2 < 0 ? "SKIP" : "PASS",
                0, out, 1);
    /* Measured on PSP-3001 6.6.1: releasing while samples are still queued returns
     * 0x80268002 (busy) and leaves Output2 reserved. Drain first, bounded at 100 ms. */
    uint32_t drain_waits = 0;
    while (out2 >= 0 && sceAudioOutput2GetRestSample() > 0 && drain_waits < 100u) {
        sceKernelDelayThread(1000);
        ++drain_waits;
    }
    const uint32_t out2_release = out2 < 0 ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioOutput2Release();
    out[0] = out2_release;
    audio_defer(emulated, "audio-out2-release",
                out2 < 0 ? "SKIP" : (int32_t)out2_release >= 0 ? "PASS" : "FAIL",
                out2_release, out, 1);

    const int src = sceAudioSRCChReserve(AUDIO_QUERY_SAMPLES, 44100, 2);
    const uint32_t src_release = src < 0 ? AUDIO_NOT_CAPTURED
        : (uint32_t)sceAudioSRCChRelease();
    out[0] = (uint32_t)src;
    out[1] = src_release;
    audio_defer(emulated, "audio-src-reserve", src < 0 ? "FAIL" : "PASS",
                src, out, 2);

    uint32_t done_out[1] = {13u};
    defer_record("audio-done", "PASS", 0, done_out, 1);
    flush_deferred(emulated, "PSP-AUDIO-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN
#define GE_NAN_WIDTH 480u
#define GE_NAN_STRIDE 512u
#define GE_NAN_HEIGHT 272u
#define GE_NAN_BG 0xff203040u
#define GE_NAN_PIXELS (GE_NAN_STRIDE * GE_NAN_HEIGHT)

struct ge_nan_input {
    const char *name;
    uint32_t bits;
};

static const struct ge_nan_input s_ge_nan_inputs[] = {
    {"qnan", 0x7fc00000u},
    {"pinf", 0x7f800000u},
    {"ninf", 0xff800000u},
    {"nzero", 0x80000000u},
    {"denorm", 0x00000001u},
};
static uint32_t s_ge_nan_list[4096] __attribute__((aligned(64)));
static uint32_t s_ge_nan_vertices[18] __attribute__((aligned(64)));
static void *s_ge_nan_frame;

static uint32_t ge_nan_vadd_zero(uint32_t input) {
    uint32_t result;
    const uint32_t zero = 0u;
    __asm__ volatile(
        "mtv %1, S000\n"
        "mtv %2, S001\n"
        "vadd.s S002, S000, S001\n"
        "mfv %0, S002\n"
        : "=r"(result) : "r"(input), "r"(zero) : "memory");
    return result;
}

static uint32_t ge_nan_vmul_one(uint32_t input) {
    uint32_t result;
    const uint32_t one = 0x3f800000u;
    __asm__ volatile(
        "mtv %1, S000\n"
        "mtv %2, S001\n"
        "vmul.s S002, S000, S001\n"
        "mfv %0, S002\n"
        : "=r"(result) : "r"(input), "r"(one) : "memory");
    return result;
}

static uint32_t ge_nan_hash_pixels(uint32_t *changed_pixels) {
    const uintptr_t physical = (uintptr_t)sceGeEdramGetAddr()
        + (uintptr_t)s_ge_nan_frame;
    const volatile uint32_t *pixels =
        (const volatile uint32_t *)(physical | 0x40000000u);
    uint32_t hash = 2166136261u;
    uint32_t changed = 0;
    for (uint32_t y = 0; y < GE_NAN_HEIGHT; ++y) {
        for (uint32_t x = 0; x < GE_NAN_WIDTH; ++x) {
            const uint32_t pixel = pixels[y * GE_NAN_STRIDE + x];
            hash = (hash ^ pixel) * 16777619u;
            changed += pixel != GE_NAN_BG;
        }
    }
    *changed_pixels = changed;
    return hash;
}

static int ge_nan_render(uint32_t mode, uint32_t bits, int *timed_out, uint32_t *hash,
                         uint32_t *changed) {
    static const uint32_t sane_pos[9] = {
        0xbf000000u, 0xbf000000u, 0x00000000u,
        0x3f000000u, 0xbf000000u, 0x00000000u,
        0x00000000u, 0x3f000000u, 0x00000000u,
    };
    static const uint32_t sane_norm[9] = {
        0x00000000u, 0x00000000u, 0x3f800000u,
        0x00000000u, 0x00000000u, 0x3f800000u,
        0x00000000u, 0x00000000u, 0x3f800000u,
    };
    int finish_rc;
    int sync_rc;

    if (sceGuStart(GU_DIRECT, s_ge_nan_list) < 0) return -1;
    sceGuDrawBuffer(GU_PSM_8888, s_ge_nan_frame, GE_NAN_STRIDE);
    sceGuOffset(2048u - (GE_NAN_WIDTH / 2u), 2048u - (GE_NAN_HEIGHT / 2u));
    sceGuViewport(2048, 2048, GE_NAN_WIDTH, GE_NAN_HEIGHT);
    sceGuScissor(0, 0, GE_NAN_WIDTH, GE_NAN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_LIGHTING);
    sceGuClearColor(GE_NAN_BG);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGuColor(0xffffffffu);

    if (mode == 0u) {
        static const uint32_t screen[9] = {
            0x43000000u, 0x43000000u, 0x00000000u,
            0x43700000u, 0x43000000u, 0x00000000u,
            0x43500000u, 0x43880000u, 0x00000000u,
        };
        memcpy(s_ge_nan_vertices, screen, sizeof(screen));
        /* Vertex 0's screen x: GU_TRANSFORM_2D consumes x and y, not z. */
        memcpy(&s_ge_nan_vertices[0], &bits, sizeof(bits));
        sceKernelDcacheWritebackRange(s_ge_nan_vertices,
                                      sizeof(s_ge_nan_vertices));
        sceGuDrawArray(GU_TRIANGLES,
                       GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       3, NULL, s_ge_nan_vertices);
    } else {
        for (uint32_t vertex = 0; vertex < 3u; ++vertex) {
            const uint32_t base = vertex * 6u;
            s_ge_nan_vertices[base + 0u] = sane_norm[vertex * 3u + 0u];
            s_ge_nan_vertices[base + 1u] = sane_norm[vertex * 3u + 1u];
            s_ge_nan_vertices[base + 2u] = sane_norm[vertex * 3u + 2u];
            s_ge_nan_vertices[base + 3u] = sane_pos[vertex * 3u + 0u];
            s_ge_nan_vertices[base + 4u] = sane_pos[vertex * 3u + 1u];
            s_ge_nan_vertices[base + 5u] = sane_pos[vertex * 3u + 2u];
        }
        if (mode == 1u) {
            /* Vertex 1's position x (normal is [6..8], position [9..11]). */
            s_ge_nan_vertices[6u + 3u] = bits;
            sceKernelDcacheWritebackRange(s_ge_nan_vertices,
                                          sizeof(s_ge_nan_vertices));
            sceGuDrawArray(GU_TRIANGLES,
                           GU_NORMAL_32BITF | GU_VERTEX_32BITF |
                               GU_TRANSFORM_3D,
                           3, NULL, s_ge_nan_vertices);
        } else {
            ScePspFVector3 direction = {-0.25f, -0.5f, -1.0f};
            s_ge_nan_vertices[6u] = bits;
            sceGuEnable(GU_LIGHTING);
            sceGuEnable(GU_LIGHT0);
            sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &direction);
            sceGuLightColor(0, GU_DIFFUSE, 0xffffffffu);
            sceGuAmbientColor(0xff202020u);
            sceGuColorMaterial(GU_DIFFUSE);
            sceGuMaterial(GU_DIFFUSE, 0xffffffffu);
            sceKernelDcacheWritebackRange(s_ge_nan_vertices,
                                          sizeof(s_ge_nan_vertices));
            sceGuDrawArray(GU_TRIANGLES,
                           GU_NORMAL_32BITF | GU_VERTEX_32BITF |
                               GU_TRANSFORM_3D,
                           3, NULL, s_ge_nan_vertices);
        }
    }
    finish_rc = sceGuFinish();
    /* Bounded replacement for sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE),
       which blocks in sceGeDrawSync(0).  On a timeout the result is the last
       observed draw state and the queues are reset so the next cell starts
       from an idle GE. */
    const GeIdleWait drawn = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);
    sync_rc = drawn.draw_state;
    *timed_out = !drawn.idle;
    if (!drawn.idle) {
        GeIdleWait after_reset;
        (void)ge_reset_queues(&after_reset);
    }
    *hash = ge_nan_hash_pixels(changed);
    return finish_rc < 0 ? finish_rc : sync_rc;
}

static void run_ge_nan(int emulated) {
    const int init_rc = sceGuInit();
    s_ge_nan_frame = guGetStaticVramBuffer(
        GE_NAN_STRIDE, GE_NAN_HEIGHT, GU_PSM_8888);
    /* guGetStaticVramBuffer returns a VRAM offset; the first buffer is offset 0, so a
     * NULL test would refuse it (measured: every render cell SKIPped on hardware). */
    /* The framebuffer must lie inside the 2 MiB of eDRAM the pixel scan reads. */
    const int ready = init_rc >= 0 &&
        (uintptr_t)s_ge_nan_frame + GE_NAN_PIXELS * 4u <= 0x00200000u;
    for (size_t i = 0; i < sizeof(s_ge_nan_inputs) / sizeof(s_ge_nan_inputs[0]); ++i) {
        const struct ge_nan_input *input = &s_ge_nan_inputs[i];
        char case_id[48];
        uint32_t out[4];
        out[0] = input->bits;
        out[1] = ge_nan_vadd_zero(input->bits);
        out[2] = ge_nan_vmul_one(input->bits);
        snprintf(case_id, sizeof(case_id), "ge-nan-vfpu-%s", input->name);
        defer_record(case_id, "PASS", 0u, out, 3);

        for (uint32_t mode = 0; mode < 3u; ++mode) {
            const char *const mode_names[] = {"screen2d", "clip3d", "litnormal"};
            uint32_t hash = 0u;
            uint32_t changed = 0u;
            int timed_out = 0;
            const int render_rc = ready
                ? ge_nan_render(mode, input->bits, &timed_out, &hash, &changed)
                : init_rc;
            out[0] = input->bits;
            out[1] = hash;
            out[2] = changed;
            out[3] = ready ? (uint32_t)render_rc : (uint32_t)init_rc;
            snprintf(case_id, sizeof(case_id), "ge-nan-%s-%s",
                     mode_names[mode], input->name);
            defer_record(case_id,
                         !ready ? "SKIP"
                         : timed_out ? "TIMEOUT"
                         : render_rc >= 0 ? "PASS" : "FAIL",
                         (uint32_t)render_rc, out, 4);
        }
    }
    const uint32_t done_out[1] = {20u};
    defer_record("ge-nan-done", "PASS", 0u, done_out, 1);
    flush_deferred(emulated, "PSP-GE-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO
#define DELAY_ZERO_NOT_CAPTURED 0xffffffffu

static volatile uint32_t s_delay_zero_worker_runs;
static volatile uint32_t s_delay_zero_callback_calls;

static int delay_zero_worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    ++s_delay_zero_worker_runs;
    return 0;
}

static int delay_zero_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    ++s_delay_zero_callback_calls;
    return 0;
}

static void run_delay_zero_trial(int emulated, int callback_aware) {
    SceKernelThreadInfo main_info;
    SceKernelThreadInfo worker_info;
    memset(&main_info, 0, sizeof(main_info));
    memset(&worker_info, 0, sizeof(worker_info));
    main_info.size = sizeof(main_info);
    worker_info.size = sizeof(worker_info);

    s_delay_zero_worker_runs = 0;
    s_delay_zero_callback_calls = 0;
    const SceUID main_tid = sceKernelGetThreadId();
    const int main_status_rc = sceKernelReferThreadStatus(main_tid, &main_info);
    const int callback = sceKernelCreateCallback(
        "oracle-delay-zero", delay_zero_callback, NULL);
    const int notify_rc = callback < 0 ? callback
        : sceKernelNotifyCallback(callback, 0x5a);
    const SceUID worker = main_status_rc < 0 ? main_status_rc
        : sceKernelCreateThread("oracle-delay-peer", delay_zero_worker,
                                main_info.currentPriority, 0x1000, 0, NULL);
    const int start_rc = worker < 0 ? (int)worker
        : sceKernelStartThread(worker, 0, NULL);
    const uint32_t runs_before = s_delay_zero_worker_runs;
    const int worker_status_rc = worker < 0 ? (int)worker
        : sceKernelReferThreadStatus(worker, &worker_info);
    const int worker_priority_before = worker_status_rc == 0
        ? worker_info.currentPriority : (int)DELAY_ZERO_NOT_CAPTURED;
    const int worker_status_before = worker_status_rc == 0
        ? worker_info.status : (int)DELAY_ZERO_NOT_CAPTURED;
    const int ready = main_status_rc == 0 && callback >= 0 && notify_rc == 0 &&
        worker >= 0 && start_rc == 0 && worker_status_rc == 0 &&
        worker_status_before == PSP_THREAD_READY && runs_before == 0u &&
        s_delay_zero_callback_calls == 0u &&
        worker_priority_before == main_info.currentPriority;

    const uint32_t callbacks_before = s_delay_zero_callback_calls;
    uint32_t delay_rc = DELAY_ZERO_NOT_CAPTURED;
    uint32_t elapsed = DELAY_ZERO_NOT_CAPTURED;
    if (ready) {
        const uint32_t start = sceKernelGetSystemTimeLow();
        const int rc = callback_aware
            ? sceKernelDelayThreadCB(0u) : sceKernelDelayThread(0u);
        elapsed = (uint32_t)(sceKernelGetSystemTimeLow() - start);
        delay_rc = (uint32_t)rc;
    }
    const uint32_t runs_after = s_delay_zero_worker_runs;
    const uint32_t callbacks_after = s_delay_zero_callback_calls;

    /* Drain a callback that DelayThread(0) may have left pending, then remove
       only the thread and callback owned by this probe. */
    if (callback >= 0) sceKernelCheckCallback();
    const int cleanup_rc = worker < 0 ? worker
        : sceKernelTerminateDeleteThread(worker);
    if (callback >= 0) sceKernelDeleteCallback(callback);

    uint32_t out[10] = {
        runs_before,
        runs_after,
        (uint32_t)notify_rc,
        callbacks_before,
        callbacks_after,
        elapsed,
        (uint32_t)main_info.currentPriority,
        (uint32_t)worker_priority_before,
        (uint32_t)worker_status_before,
        (uint32_t)cleanup_rc,
    };
    const char *case_id = callback_aware
        ? "delay-threadcb-zero" : "delay-thread-zero";
    defer_record(case_id, ready ? "PASS" : "SKIP", delay_rc, out, 10);
    (void)emulated;
}

static void run_delay_zero(int emulated) {
    run_delay_zero_trial(emulated, 1);
    run_delay_zero_trial(emulated, 0);
    const uint32_t done_out[1] = {2u};
    defer_record("delay-zero-done", "PASS", 0u, done_out, 1);
    flush_deferred(emulated, "PSP-KERNEL-002");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
/* 4 measurement cells + 1 completion sentinel. */
_Static_assert(DEFERRED_MAX_RECORDS >= 5, "cache-alias needs 5 deferred slots");

static uint32_t s_cache_buf[64] __attribute__((aligned(64)));

static void run_cache_alias(int emulated) {
    uint32_t out[6];
    volatile uint32_t *c_ptr = s_cache_buf;
    volatile uint32_t *u_ptr = (volatile uint32_t *)((uintptr_t)s_cache_buf | 0x40000000u);

    /* Cell 1: cache-alias-init (Uncached write visible after invalidate) */
    memset(out, 0xFF, sizeof(out));
    u_ptr[0] = 0x11223344u;
    sceKernelDcacheInvalidateRange((void *)c_ptr, 64);
    uint32_t c_read1 = c_ptr[0];
    out[0] = u_ptr[0];
    out[1] = c_read1;
    out[2] = (c_read1 == 0x11223344u) ? 1u : 0u;
    defer_record("cache-alias-init",
                 c_read1 == 0x11223344u ? "PASS" : "FAIL", c_read1, out, 3);

    /* Cell 2: cache-writeback-contrast (Cached write vs uncached visibility before/after WB) */
    memset(out, 0xFF, sizeof(out));
    c_ptr[0] = 0x55667788u;
    uint32_t u_before = u_ptr[0];
    sceKernelDcacheWritebackRange((void *)c_ptr, 64);
    uint32_t u_after = u_ptr[0];
    out[0] = u_before;
    out[1] = u_after;
    out[2] = (u_after == 0x55667788u) ? 1u : 0u;
    defer_record("cache-writeback-contrast",
                 u_after == 0x55667788u ? "PASS" : "FAIL", u_after, out, 3);

    /* Cell 3: cache-inval-contrast (Uncached write vs stale cached read before/after inval) */
    memset(out, 0xFF, sizeof(out));
    u_ptr[0] = 0x99AABBCCu;
    uint32_t c_stale = c_ptr[0];
    sceKernelDcacheInvalidateRange((void *)c_ptr, 64);
    uint32_t c_fresh = c_ptr[0];
    out[0] = c_stale;
    out[1] = c_fresh;
    out[2] = (c_fresh == 0x99AABBCCu) ? 1u : 0u;
    defer_record("cache-inval-contrast",
                 c_fresh == 0x99AABBCCu ? "PASS" : "FAIL", c_fresh, out, 3);

    /* Cell 4: cache-wball (Writeback all lines) */
    memset(out, 0xFF, sizeof(out));
    c_ptr[1] = 0xDEADBEEFu;
    sceKernelDcacheWritebackAll();
    uint32_t u_wball = u_ptr[1];
    out[0] = u_wball;
    out[1] = (u_wball == 0xDEADBEEFu) ? 1u : 0u;
    defer_record("cache-wball",
                 u_wball == 0xDEADBEEFu ? "PASS" : "FAIL", u_wball, out, 2);

    /* Every dcache/alias measurement is now complete.  Emission below is the
       first host0 or stdout activity since the probe began, so no line
       residency observed above was perturbed by logging. */
    const uint32_t done_out[1] = {(uint32_t)s_deferred_count};
    defer_record("cache-done", "PASS", 0, done_out, 1);

    flush_deferred(emulated, "PSP-CACHE-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MODEL_PROFILE
/* The PSPSDK/kubridge user bridge exposes the PspModel ordinal.  It is kept
   as a scalar record beside the firmware word; the host decoder owns the
   generation-to-retail-family presentation and rejects unknown ordinals. */
static void run_model_profile(int emulated) {
    const int model_code = kuKernelGetModel();
    const uint32_t firmware = (uint32_t)sceKernelDevkitVersion();
    const uint32_t cpu_mhz = (uint32_t)scePowerGetCpuClockFrequencyInt();
    const uint32_t out[] = {
        (uint32_t)model_code,
        firmware,
        cpu_mhz,
    };
    emit_record_extended(emulated, "PSP-SYSTEM-001", "model-profile",
                         model_code < 0 ? "ERROR" : "PASS",
                         (uint32_t)model_code, out,
                         sizeof(out) / sizeof(out[0]));
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_LATE
/* D1 -- what does a LATE display wait do?
 *
 * Nakagawa's scheduler carries a per-thread `vbl_seen` latch: if a VBLANK was
 * delivered since the calling thread last completed a display wait, the wait
 * returns immediately and consumes the missed edge.  The runtime's own comment
 * calls that "a Nakagawa pacing artifact", but nothing in this project has ever
 * measured the normal-context blocking behaviour -- docs/PSP_INTR_WAITS_MATRIX.md
 * records `hardware = unknown / WOULD_BLOCK control` for both NIDs.  Only the
 * error cells (interrupts disabled, dispatch disabled) are measured.
 *
 * The three candidate semantics this case separates:
 *
 *   A  remembered/missed-edge: a late call can return immediately because an
 *      edge elapsed since the caller last waited.
 *   B  next-edge: the call always waits for the next appropriate boundary,
 *      regardless of how many edges were missed.
 *   C  the two NIDs differ -- sceDisplayWaitVblank has an in-vblank fast return
 *      that sceDisplayWaitVblankStart does not.
 *
 * Method.  Phase-align to a boundary, busy-spin a controlled fraction of a
 * period WITHOUT any voluntary yield (so no syscall can absorb the edge), then
 * time the call under test.  Under (A) a late call costs ~0 us and advances
 * VCOUNT by 0.  Under (B) it costs the remainder of the period and advances
 * VCOUNT by 1.  The period is calibrated on the same device in the same run;
 * nothing here assumes 60000/1001.
 *
 * The offsets deliberately straddle one and two periods so "several host
 * periods elapsed while the caller was busy" is covered, not just a narrow
 * late window.  Every spin is bounded by both elapsed system time and an
 * iteration cap, so a stopped clock degrades to a finite record, never a hang. */

#define DW_TRIALS 48

/* Requested spin, in 1/8ths of a calibrated period, measured from the aligned
 * boundary. 2/8 and 6/8 are ordinary sub-period lateness; 10/8 and 14/8 cross
 * one boundary; 20/8 crosses two. */
static const uint32_t k_dw_eighths[] = { 2u, 6u, 10u, 14u, 20u };
#define DW_OFFSETS ((int)(sizeof(k_dw_eighths) / sizeof(k_dw_eighths[0])))

/* api: 0 = sceDisplayWaitVblankStart, 1 = sceDisplayWaitVblank */
static int dw_call(int api) {
    return api ? sceDisplayWaitVblank() : sceDisplayWaitVblankStart();
}

static const char *dw_api_name(int api) {
    return api ? "waitvblank" : "waitvblankstart";
}

/* One (api, offset) cell: DW_TRIALS timed late calls. */
static void dw_run_cell(int emulated, int api, uint32_t want_us, uint32_t eighths,
                        uint32_t period_ns) {
    uint32_t trials = 0;
    uint32_t w_min = 0xffffffffu, w_max = 0, w_sum = 0;
    uint32_t vd_min = 0xffffffffu, vd_max = 0, vd_sum = 0;
    uint32_t n_vd0 = 0, n_vd1 = 0, n_vd2plus = 0;
    uint32_t n_immediate = 0, n_blocked = 0;
    uint32_t span_min = 0xffffffffu, span_max = 0;
    uint32_t n_invbl_at_call = 0;
    uint32_t rc_last = 0, n_rc_nonzero = 0;

    /* "immediate" is a generous threshold: an eighth of a real period. A
     * next-edge return from any of these offsets costs far more than that. */
    const uint32_t immediate_us = period_ns ? (uint32_t)(period_ns / 8000u) : 2000u;

    for (uint32_t k = 0; k < DW_TRIALS; k++) {
        sceDisplayWaitVblankStart();               /* phase-align */
        const uint32_t st0 = sceKernelGetSystemTimeLow();
        const uint32_t span = spin_us(st0, want_us, NULL);

        const uint32_t vcA = sceDisplayGetVcount();
        const int invbl = sceDisplayIsVblank();
        const uint32_t tA = sceKernelGetSystemTimeLow();
        const int rc = dw_call(api);
        const uint32_t tB = sceKernelGetSystemTimeLow();
        const uint32_t vcB = sceDisplayGetVcount();

        const uint32_t wait_us = (uint32_t)(tB - tA);
        const uint32_t vd = vcB - vcA;

        trials++;
        rc_last = (uint32_t)rc;
        if (rc != 0) n_rc_nonzero++;
        if (invbl) n_invbl_at_call++;
        if (wait_us < w_min) w_min = wait_us;
        if (wait_us > w_max) w_max = wait_us;
        w_sum += wait_us;
        if (vd < vd_min) vd_min = vd;
        if (vd > vd_max) vd_max = vd;
        vd_sum += vd;
        if (vd == 0u) n_vd0++;
        else if (vd == 1u) n_vd1++;
        else n_vd2plus++;
        if (wait_us <= immediate_us) n_immediate++; else n_blocked++;
        if (span < span_min) span_min = span;
        if (span > span_max) span_max = span;
    }

    if (w_min == 0xffffffffu) w_min = 0;
    if (vd_min == 0xffffffffu) vd_min = 0;
    if (span_min == 0xffffffffu) span_min = 0;

    char case_id[64];
    snprintf(case_id, sizeof(case_id), "late-%s-%ueighths",
             dw_api_name(api), (unsigned int)eighths);

    const uint32_t out[] = {
        (uint32_t)api, eighths, want_us, trials, period_ns,
        w_min, w_max, w_sum,
        vd_min, vd_max, vd_sum,
        n_vd0, n_vd1, n_vd2plus,
        n_immediate, n_blocked, immediate_us,
        span_min, span_max,
        n_invbl_at_call, n_rc_nonzero, rc_last,
    };
    emit_record_extended(emulated, "PSP-DISPLAY-002", case_id,
                         trials == DW_TRIALS ? "PASS" : "FAIL", rc_last,
                         out, sizeof(out) / sizeof(out[0]));
}

/* The in-vblank cell: instead of a fixed offset, spin until the display
 * reports it is INSIDE the vblank interval, then call immediately. This is the
 * only phase at which hypothesis (C) can show a difference between the two
 * NIDs, so it is measured separately rather than hoped for inside the sweep. */
static void dw_run_invblank_cell(int emulated, int api, uint32_t period_ns) {
    uint32_t trials = 0, reached = 0;
    uint32_t w_min = 0xffffffffu, w_max = 0, w_sum = 0;
    uint32_t n_vd0 = 0, n_vd1 = 0, n_vd2plus = 0;
    uint32_t n_immediate = 0, n_blocked = 0;
    uint32_t rc_last = 0, n_rc_nonzero = 0;
    const uint32_t immediate_us = period_ns ? (uint32_t)(period_ns / 8000u) : 2000u;

    for (uint32_t k = 0; k < DW_TRIALS; k++) {
        sceDisplayWaitVblankStart();
        /* Bounded hunt for the in-vblank window. The cap is an iteration cap,
         * never the expected exit; a miss is recorded rather than retried
         * forever. */
        int inside = 0;
        for (uint32_t i = 0; i < 4000000u; i++) {
            if (sceDisplayIsVblank()) { inside = 1; break; }
            s_spin_sink = i;
        }
        trials++;
        if (!inside) continue;
        reached++;

        const uint32_t vcA = sceDisplayGetVcount();
        const uint32_t tA = sceKernelGetSystemTimeLow();
        const int rc = dw_call(api);
        const uint32_t tB = sceKernelGetSystemTimeLow();
        const uint32_t vcB = sceDisplayGetVcount();

        const uint32_t wait_us = (uint32_t)(tB - tA);
        const uint32_t vd = vcB - vcA;
        rc_last = (uint32_t)rc;
        if (rc != 0) n_rc_nonzero++;
        if (wait_us < w_min) w_min = wait_us;
        if (wait_us > w_max) w_max = wait_us;
        w_sum += wait_us;
        if (vd == 0u) n_vd0++; else if (vd == 1u) n_vd1++; else n_vd2plus++;
        if (wait_us <= immediate_us) n_immediate++; else n_blocked++;
    }
    if (w_min == 0xffffffffu) w_min = 0;

    char case_id[64];
    snprintf(case_id, sizeof(case_id), "invblank-%s", dw_api_name(api));
    const uint32_t out[] = {
        (uint32_t)api, 0xffffffffu, 0u, trials, period_ns,
        w_min, w_max, w_sum,
        0u, 0u, 0u,
        n_vd0, n_vd1, n_vd2plus,
        n_immediate, n_blocked, immediate_us,
        reached, 0u,
        reached, n_rc_nonzero, rc_last,
    };
    emit_record_extended(emulated, "PSP-DISPLAY-002", case_id,
                         reached ? "PASS" : "SKIP", rc_last,
                         out, sizeof(out) / sizeof(out[0]));
}

static void run_display_wait_late(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);

    /* Publish the calibration so every downstream number is interpretable
     * without assuming a refresh rate. */
    {
        const uint32_t out[] = { period_ns, calib_frames, (uint32_t)DW_TRIALS,
                                 (uint32_t)DW_OFFSETS,
                                 (uint32_t)scePowerGetCpuClockFrequencyInt() };
        emit_record_extended(emulated, "PSP-DISPLAY-002", "calibration",
                             period_ns ? "PASS" : "FAIL", period_ns,
                             out, sizeof(out) / sizeof(out[0]));
    }
    if (!period_ns) return;

    for (int api = 0; api < 2; api++) {
        for (int d = 0; d < DW_OFFSETS; d++) {
            const uint32_t eighths = k_dw_eighths[d];
            const uint32_t want_us =
                (uint32_t)(((uint64_t)period_ns * eighths) / (8u * 1000u));
            dw_run_cell(emulated, api, want_us, eighths, period_ns);
        }
        dw_run_invblank_cell(emulated, api, period_ns);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_PRIORITY
/* D2 -- does an unrelated READY thread change the display wait?
 *
 * The rejected candidate 7a4fafc made sched_wait_vblank() consult the whole TCB
 * table and block the caller whenever ANY other thread was TH_READY, so that a
 * lower-priority asset loader would be scheduled. That fixed the game. It is
 * only defensible if the hardware syscall is itself readiness-dependent, so
 * this case measures exactly that, and separates it from the thing that is
 * genuinely true on hardware: a thread that really blocks really does hand the
 * CPU to a lower-priority peer.
 *
 * Two runs, identical high-priority work:
 *
 *   CONTROL     high-priority thread only.
 *   EXPERIMENT  same thread, plus an always-runnable lower-priority thread that
 *               never performs a blocking call.
 *
 * Per iteration the high thread records the low counter at three points:
 *
 *   c0  before the display wait
 *   c1  after it            -> (c1-c0) is progress made while we were BLOCKED
 *   c2  after a pure CPU spin that issues no blocking call
 *                           -> (c2-c1) is progress made while we were RUNNABLE
 *
 * Strict priority predicts (c2-c1) == 0: a busy higher-priority thread starves
 * a lower-priority one, with no aging. A readiness-dependent syscall would show
 * up as a difference in the WAIT DURATION or total wall time between CONTROL
 * and EXPERIMENT -- same caller, same display state, different answer purely
 * because another thread exists. */

#define DWP_ITERS      120
#define DWP_HIGH_PRIO  0x30
#define DWP_LOW_PRIO   0x50

static volatile uint32_t g_dwp_low_counter;
static volatile int      g_dwp_stop;

/* Always runnable, never blocks: no delay, no wait, no display call. Under
 * strict priority it is legitimate for this to make no progress at all. */
static int dwp_low_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (!g_dwp_stop) {
        g_dwp_low_counter++;
    }
    return 0;
}

static struct {
    uint32_t iters;
    uint32_t w_min, w_max, w_sum;
    uint32_t blocked_delta_sum, runnable_delta_sum;
    uint32_t n_runnable_nonzero;
    uint32_t wall_us;
    uint32_t vc_total;
    uint32_t spin_want_us;
} g_dwp;

static int dwp_high_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    const uint32_t spin_want = g_dwp.spin_want_us;
    uint32_t w_min = 0xffffffffu, w_max = 0, w_sum = 0;
    uint32_t bsum = 0, rsum = 0, rnz = 0;

    sceDisplayWaitVblankStart();                    /* phase-align */
    const uint32_t vc_start = sceDisplayGetVcount();
    const uint32_t wall0 = sceKernelGetSystemTimeLow();

    for (uint32_t i = 0; i < DWP_ITERS; i++) {
        const uint32_t c0 = g_dwp_low_counter;
        const uint32_t tA = sceKernelGetSystemTimeLow();
        sceDisplayWaitVblankStart();
        const uint32_t tB = sceKernelGetSystemTimeLow();
        const uint32_t c1 = g_dwp_low_counter;

        /* Pure CPU. Issues sceKernelGetSystemTimeLow, which is a clock read, not
         * a blocking call: it must not hand the CPU to a weaker thread. */
        spin_us(tB, spin_want, NULL);
        const uint32_t c2 = g_dwp_low_counter;

        const uint32_t w = (uint32_t)(tB - tA);
        if (w < w_min) w_min = w;
        if (w > w_max) w_max = w;
        w_sum += w;
        bsum += (uint32_t)(c1 - c0);
        const uint32_t rd = (uint32_t)(c2 - c1);
        rsum += rd;
        if (rd) rnz++;
    }

    const uint32_t wall1 = sceKernelGetSystemTimeLow();
    const uint32_t vc_end = sceDisplayGetVcount();
    if (w_min == 0xffffffffu) w_min = 0;

    g_dwp.iters = DWP_ITERS;
    g_dwp.w_min = w_min; g_dwp.w_max = w_max; g_dwp.w_sum = w_sum;
    g_dwp.blocked_delta_sum = bsum;
    g_dwp.runnable_delta_sum = rsum;
    g_dwp.n_runnable_nonzero = rnz;
    g_dwp.wall_us = (uint32_t)(wall1 - wall0);
    g_dwp.vc_total = vc_end - vc_start;
    return 0;
}

/* One run. with_low != 0 creates the always-runnable lower-priority peer. */
static void dwp_run(int emulated, int with_low, uint32_t period_ns) {
    memset((void *)&g_dwp, 0, sizeof(g_dwp));
    g_dwp.spin_want_us = period_ns ? (uint32_t)(period_ns / 4000u) : 4000u;
    g_dwp_low_counter = 0;
    g_dwp_stop = 0;

    SceUID low = -1;
    int low_started = 0;
    if (with_low) {
        low = sceKernelCreateThread("dwp_low", dwp_low_thread, DWP_LOW_PRIO,
                                    0x1000, THREAD_ATTR_USER, NULL);
        if (low >= 0) {
            const int low_start = sceKernelStartThread(low, 0, NULL);
            if (low_start >= 0) low_started = 1;
        }
    }

    uint32_t setup_ok = 0;
    int high_started = 0;
    SceUID high = -1;
    /* The experiment is meaningful only with its always-runnable peer.  Do not
     * silently turn a failed peer setup into the high-only control case. */
    if (!with_low || low_started) {
        high = sceKernelCreateThread("dwp_high", dwp_high_thread, DWP_HIGH_PRIO,
                                     0x2000, THREAD_ATTR_USER, NULL);
        if (high >= 0) {
            const int high_start = sceKernelStartThread(high, 0, NULL);
            if (high_start >= 0) {
                high_started = 1;
                setup_ok = 1u;
                sceKernelWaitThreadEnd(high, NULL);
                sceKernelDeleteThread(high);
            } else {
                sceKernelDeleteThread(high);
                high = -1;
            }
        }
    }

    g_dwp_stop = 1;
    if (low >= 0) {
        if (!low_started) {
            sceKernelDeleteThread(low);
        } else {
            /* The low thread exits on the flag; the join is bounded by a terminate
             * fallback so a probe can never be left with a spinning thread. */
            SceUInt join_us = 2000000u;
            if (sceKernelWaitThreadEnd(low, &join_us) < 0) {
                sceKernelTerminateDeleteThread(low);
            } else {
                sceKernelDeleteThread(low);
            }
        }
    }

    const uint32_t out[] = {
        (uint32_t)with_low, setup_ok, g_dwp.iters, period_ns, g_dwp.spin_want_us,
        g_dwp.w_min, g_dwp.w_max, g_dwp.w_sum,
        g_dwp.wall_us, g_dwp.vc_total,
        g_dwp_low_counter,
        g_dwp.blocked_delta_sum,
        g_dwp.runnable_delta_sum,
        g_dwp.n_runnable_nonzero,
        (uint32_t)DWP_HIGH_PRIO, (uint32_t)DWP_LOW_PRIO,
    };
    emit_record_extended(emulated, "PSP-DISPLAY-003",
                         with_low ? "priority-experiment" : "priority-control",
                         setup_ok && high_started && g_dwp.iters == DWP_ITERS ? "PASS" : "FAIL",
                         setup_ok, out, sizeof(out) / sizeof(out[0]));
}

static void run_display_wait_priority(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);
    {
        const uint32_t out[] = { period_ns, calib_frames, (uint32_t)DWP_ITERS,
                                 (uint32_t)DWP_HIGH_PRIO, (uint32_t)DWP_LOW_PRIO };
        emit_record_extended(emulated, "PSP-DISPLAY-003", "calibration",
                             period_ns ? "PASS" : "FAIL", period_ns,
                             out, sizeof(out) / sizeof(out[0]));
    }
    if (!period_ns) return;
    dwp_run(emulated, 0, period_ns);   /* control first */
    dwp_run(emulated, 1, period_ns);   /* then the always-ready peer */
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_VBLANK_WINDOW
/* D3 -- where in the period is the vblank interval, and how long is it?
 *
 * D1 already proves the interval BEGINS at the vblank start edge rather than
 * ending at it: a sceDisplayWaitVblankStart issued while sceDisplayIsVblank()
 * was true waited a FULL period, which is only possible if the caller had just
 * crossed a start edge. Nakagawa currently models the window at the opposite
 * end of the period (sched_display_is_vblank() tests the LAST 1500 us before
 * the next edge), so the placement and the width both need a measurement rather
 * than a constant nobody sourced.
 *
 * Method: align to the edge, then poll sceDisplayIsVblank() and timestamp the
 * transition to false. Also record hcount at entry and exit so the window can be
 * expressed in the display's own units, not just microseconds. All loops carry
 * an iteration cap so a stuck flag yields a finite record. */

#define VW_TRIALS 48
#define VW_POLL_CAP 4000000u

static void run_display_vblank_window(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);

    uint32_t trials = 0, clean = 0;
    uint32_t d_min = 0xffffffffu, d_max = 0, d_sum = 0;
    uint32_t entry_true = 0;                 /* IsVblank already true right after the edge */
    uint32_t hc_in_min = 0xffffffffu, hc_in_max = 0;
    uint32_t hc_out_min = 0xffffffffu, hc_out_max = 0;
    uint32_t lat_min = 0xffffffffu, lat_max = 0;   /* edge -> first sample latency */

    for (uint32_t k = 0; k < VW_TRIALS; k++) {
        sceDisplayWaitVblankStart();
        const uint32_t t_edge = sceKernelGetSystemTimeLow();
        const int first = sceDisplayIsVblank();
        const uint32_t hc_in = (uint32_t)sceDisplayGetCurrentHcount();
        const uint32_t t_first = sceKernelGetSystemTimeLow();
        trials++;
        if (first) entry_true++;

        /* Poll to the falling edge. */
        uint32_t t_fall = t_first;
        uint32_t hc_out = hc_in;
        int fell = 0;
        for (uint32_t i = 0; i < VW_POLL_CAP; i++) {
            if (!sceDisplayIsVblank()) {
                t_fall = sceKernelGetSystemTimeLow();
                hc_out = (uint32_t)sceDisplayGetCurrentHcount();
                fell = 1;
                break;
            }
            s_spin_sink = i;
        }
        if (!first || !fell) continue;
        clean++;

        const uint32_t dur = (uint32_t)(t_fall - t_edge);
        const uint32_t lat = (uint32_t)(t_first - t_edge);
        if (dur < d_min) d_min = dur;
        if (dur > d_max) d_max = dur;
        d_sum += dur;
        if (lat < lat_min) lat_min = lat;
        if (lat > lat_max) lat_max = lat;
        if (hc_in < hc_in_min) hc_in_min = hc_in;
        if (hc_in > hc_in_max) hc_in_max = hc_in;
        if (hc_out < hc_out_min) hc_out_min = hc_out;
        if (hc_out > hc_out_max) hc_out_max = hc_out;
    }

    if (d_min == 0xffffffffu) d_min = 0;
    if (lat_min == 0xffffffffu) lat_min = 0;
    if (hc_in_min == 0xffffffffu) hc_in_min = 0;
    if (hc_out_min == 0xffffffffu) hc_out_min = 0;

    const uint32_t out[] = {
        period_ns, calib_frames, trials, clean, entry_true,
        d_min, d_max, d_sum,
        lat_min, lat_max,
        hc_in_min, hc_in_max, hc_out_min, hc_out_max,
    };
    emit_record_extended(emulated, "PSP-DISPLAY-004", "vblank-window",
                         clean ? "PASS" : "FAIL", clean,
                         out, sizeof(out) / sizeof(out[0]));
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED
static uint32_t run_mutex_refer_unlocked_case(uint32_t *out0, uint32_t *out1,
                                              uint32_t *out2, uint32_t *out3,
                                              uint32_t *out4) {
    const SceUID self = sceKernelGetThreadId();
    SceKernelMutexInfo info1;
    SceKernelMutexInfo info2;
    SceKernelMutexInfo info3;
    memset(&info1, 0, sizeof(info1));
    memset(&info2, 0, sizeof(info2));
    memset(&info3, 0, sizeof(info3));
    info1.size = sizeof(info1);
    info2.size = sizeof(info2);
    info3.size = sizeof(info3);

    const SceUID m1 = sceKernelCreateMutex("oracle-m1", 0, 0, NULL);
    const int r1 = m1 >= 0 ? sceKernelReferMutexStatus(m1, &info1) : -1;

    const SceUID m2 = sceKernelCreateMutex("oracle-m2", 0, 1, NULL);
    const int r2 = m2 >= 0 ? sceKernelReferMutexStatus(m2, &info2) : -1;

    const int unlock2 = m2 >= 0 ? sceKernelUnlockMutex(m2, 1) : -1;
    const int r3 = m2 >= 0 ? sceKernelReferMutexStatus(m2, &info3) : -1;

    if (m1 >= 0) sceKernelDeleteMutex(m1);
    if (m2 >= 0) sceKernelDeleteMutex(m2);

    *out0 = (uint32_t)(m1 >= 0) |
            ((uint32_t)(r1 == 0) << 1) |
            ((uint32_t)(m2 >= 0) << 2) |
            ((uint32_t)(r2 == 0) << 3) |
            ((uint32_t)(unlock2 == 0) << 4) |
            ((uint32_t)(r3 == 0) << 5) |
            ((uint32_t)(info2.lockThread == self) << 6);

    *out1 = (uint32_t)info1.lockThread; /* raw lockThread when unlocked at creation */
    *out2 = (uint32_t)info2.lockThread; /* raw lockThread when locked at creation */
    *out3 = (uint32_t)info3.lockThread; /* raw lockThread when unlocked after unlock */
    *out4 = (uint32_t)self;

    return m1 >= 0 && r1 == 0 && m2 >= 0 && r2 == 0 && unlock2 == 0 && r3 == 0;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_TIMEOUT_QUANTA
static volatile SceUID s_timeout_mutex;
static volatile SceUID s_timeout_sema;
static volatile uint32_t s_timeout_out[17];

static int timeout_worker_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const uint32_t intervals[] = {1, 10, 25, 50, 100, 250, 500, 1000, 2500};
    uint32_t ret_words[9] = {0};
    uint32_t min_us[9];
    uint32_t max_us[9];
    for (int i = 0; i < 9; i++) {
        min_us[i] = 0xffffffffu;
        max_us[i] = 0;
    }

    for (int i = 0; i < 9; i++) {
        uint32_t req = intervals[i];
        for (int trial = 0; trial < 10; trial++) {
            uint32_t to = req;
            uint64_t t0 = sceKernelGetSystemTimeWide();
            int res = sceKernelLockMutex(s_timeout_mutex, 1, &to);
            uint64_t t1 = sceKernelGetSystemTimeWide();
            (void)res;
            uint32_t elapsed = (uint32_t)(t1 - t0);
            if (elapsed < min_us[i]) min_us[i] = elapsed;
            if (elapsed > max_us[i]) max_us[i] = elapsed;
            ret_words[i] = to;
        }
    }

    uint32_t cb_to = 250;
    uint64_t cb_t0 = sceKernelGetSystemTimeWide();
    int cb_res = sceKernelLockMutexCB(s_timeout_mutex, 1, &cb_to);
    uint64_t cb_t1 = sceKernelGetSystemTimeWide();
    (void)cb_res;
    uint32_t cb_elapsed = (uint32_t)(cb_t1 - cb_t0);

    uint32_t early_to = 100000;
    sceKernelSignalSema(s_timeout_sema, 1);
    uint64_t early_t0 = sceKernelGetSystemTimeWide();
    int early_res = sceKernelLockMutex(s_timeout_mutex, 1, &early_to);
    uint64_t early_t1 = sceKernelGetSystemTimeWide();
    uint32_t early_elapsed = (uint32_t)(early_t1 - early_t0);

    s_timeout_out[0] = 1;
    s_timeout_out[1] = ret_words[0]; /* 1us returned toptr */
    s_timeout_out[2] = min_us[0];
    s_timeout_out[3] = max_us[0];
    s_timeout_out[4] = ret_words[2]; /* 25us returned toptr */
    s_timeout_out[5] = min_us[2];
    s_timeout_out[6] = max_us[2];
    s_timeout_out[7] = ret_words[5]; /* 250us returned toptr */
    s_timeout_out[8] = min_us[5];
    s_timeout_out[9] = max_us[5];
    s_timeout_out[10] = ret_words[7]; /* 1000us returned toptr */
    s_timeout_out[11] = min_us[7];
    s_timeout_out[12] = max_us[7];
    s_timeout_out[13] = cb_to;
    s_timeout_out[14] = cb_elapsed;
    s_timeout_out[15] = early_to;
    s_timeout_out[16] = early_elapsed;

    return (int)early_res;
}

static uint32_t run_mutex_timeout_quanta_case(uint32_t *out, size_t max_out) {
    s_timeout_mutex = sceKernelCreateMutex("oracle-m-to", 0, 1, NULL);
    s_timeout_sema = sceKernelCreateSema("oracle-s-to", 0, 0, 1, NULL);
    if (s_timeout_mutex < 0 || s_timeout_sema < 0) {
        if (s_timeout_mutex >= 0) sceKernelDeleteMutex(s_timeout_mutex);
        if (s_timeout_sema >= 0) sceKernelDeleteSema(s_timeout_sema);
        return 0;
    }

    SceUID worker = sceKernelCreateThread("oracle-w-to", timeout_worker_entry,
                                          0x20, 0x4000, 0, NULL);
    if (worker < 0) {
        sceKernelDeleteMutex(s_timeout_mutex);
        sceKernelDeleteSema(s_timeout_sema);
        return 0;
    }

    sceKernelStartThread(worker, 0, NULL);
    sceKernelWaitSema(s_timeout_sema, 1, NULL);
    sceKernelDelayThread(500);
    sceKernelUnlockMutex(s_timeout_mutex, 1);

    sceKernelWaitThreadEnd(worker, NULL);
    sceKernelDeleteThread(worker);
    sceKernelDeleteMutex(s_timeout_mutex);
    sceKernelDeleteSema(s_timeout_sema);

    for (size_t i = 0; i < 17 && i < max_out; i++) {
        out[i] = s_timeout_out[i];
    }
    return 1;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_PRIORITY_INHERITANCE
static volatile SceUID s_prio_m;
static volatile SceUID s_prio_sema_started;
static volatile SceUID s_prio_sema_unlocked;
static volatile uint32_t s_owner_self_prio;

static int prio_owner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(s_prio_m, 1, NULL);
    sceKernelSignalSema(s_prio_sema_started, 1);
    sceKernelWaitSema(s_prio_sema_unlocked, 1, NULL);
    s_owner_self_prio = (uint32_t)sceKernelGetThreadCurrentPriority();
    sceKernelUnlockMutex(s_prio_m, 1);
    return 0;
}

static int prio_waiter_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(s_prio_m, 1, NULL);
    sceKernelUnlockMutex(s_prio_m, 1);
    return 0;
}

static uint32_t run_mutex_priority_inheritance_case(uint32_t *out0, uint32_t *out1,
                                                    uint32_t *out2, uint32_t *out3,
                                                    uint32_t *out4, uint32_t *out5,
                                                    uint32_t *out6) {
    s_prio_m = sceKernelCreateMutex("oracle-m-prio", 0x100, 0, NULL);
    s_prio_sema_started = sceKernelCreateSema("oracle-s-start", 0, 0, 1, NULL);
    s_prio_sema_unlocked = sceKernelCreateSema("oracle-s-unl", 0, 0, 1, NULL);

    SceUID owner = sceKernelCreateThread("oracle-owner", prio_owner_entry, 0x30, 0x4000, 0, NULL);
    SceUID waiter = sceKernelCreateThread("oracle-waiter", prio_waiter_entry, 0x20, 0x4000, 0, NULL);

    sceKernelStartThread(owner, 0, NULL);
    sceKernelWaitSema(s_prio_sema_started, 1, NULL);

    SceKernelThreadInfo info_before;
    memset(&info_before, 0, sizeof(info_before));
    info_before.size = sizeof(info_before);
    sceKernelReferThreadStatus(owner, &info_before);

    sceKernelStartThread(waiter, 0, NULL);
    sceKernelDelayThread(1000);

    SceKernelThreadInfo info_during;
    memset(&info_during, 0, sizeof(info_during));
    info_during.size = sizeof(info_during);
    sceKernelReferThreadStatus(owner, &info_during);

    sceKernelSignalSema(s_prio_sema_unlocked, 1);
    sceKernelWaitThreadEnd(owner, NULL);
    sceKernelWaitThreadEnd(waiter, NULL);

    SceKernelThreadInfo info_after;
    memset(&info_after, 0, sizeof(info_after));
    info_after.size = sizeof(info_after);
    sceKernelReferThreadStatus(owner, &info_after);

    sceKernelDeleteThread(owner);
    sceKernelDeleteThread(waiter);
    sceKernelDeleteMutex(s_prio_m);
    sceKernelDeleteSema(s_prio_sema_started);
    sceKernelDeleteSema(s_prio_sema_unlocked);

    *out0 = 1;
    *out1 = (uint32_t)info_before.currentPriority;
    *out2 = (uint32_t)info_during.currentPriority;
    *out3 = s_owner_self_prio;
    *out4 = (uint32_t)info_after.currentPriority;
    *out5 = (uint32_t)info_during.initPriority;
    *out6 = 0;
    return 1;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_INTERRUPT_CONTEXT
/* Interrupt-state proof from user mode. This probe is a user-mode module, so
   it cannot import InterruptManagerForKernel (sceKernelIsIntrContext): the PSP
   refuses to load a user-mode PRX that imports a kernel-only library. The
   handler instead records the raw return of sceKernelIsCpuIntrEnable(), the
   user Kernel_Library export from pspintrman.h and the PSPSDK libpspuser.a
   (the kernel-alarm case samples the same call inside its alarm handler).
   A trial counts only when it is 0, meaning the handler body ran with CPU
   interrupts disabled, as the interrupt dispatcher runs it, before any mutex
   call was made. */

/* MUTEX_INTR_TRIALS: number of VBLANK firings to sample.  Each firing emits
   one protocol record with a trial-indexed case_id (mutex-interrupt-context-t00
   .. mutex-interrupt-context-t19), so the parser sees 20 independent records.
   20 trials establish reproducibility and expose any per-firing variance. */
#define MUTEX_INTR_TRIALS 20

/* Per-trial result structure written by the VBLANK sub-interrupt handler.
   All fields are set atomically from within the ISR; main thread reads them
   only after s_subintr_count is incremented and the mutex mutex cycle resets. */
typedef struct {
    uint32_t intr_enabled; /* sceKernelIsCpuIntrEnable() return value from ISR */
    uint32_t r_bad_uid;   /* LockMutex(0x7fffffff, 1, NULL)  -- bad UID         */
    uint32_t r_bad_cnt;   /* LockMutex(valid, 0, NULL)        -- bad count       */
    uint32_t r_lock;      /* LockMutex(valid_unlocked, 1, NULL)                  */
    uint32_t r_lock_cb;   /* LockMutexCB(valid_unlocked, 1, NULL)                */
    uint32_t r_try;       /* TryLockMutex(valid_unlocked, 1)                     */
    uint32_t r_unlock;    /* UnlockMutex(main_owned, 1) -- non-owner unlock      */
} IntrTrial;

static volatile int s_subintr_count;
static volatile SceUID s_intr_m_unlocked;   /* unlocked mutex (main does NOT hold) */
static volatile SceUID s_intr_m_main_owned; /* locked mutex owned by main thread    */
static IntrTrial s_trials[MUTEX_INTR_TRIALS];

/* VBLANK sub-interrupt handler.  Fires once per vertical blank (~60 Hz).
   Increments s_subintr_count only after writing all six cell results so the
   main thread can use s_subintr_count as the ready sentinel.  Samples
   sceKernelIsCpuIntrEnable() as the first call -- before any mutex
   operation -- as proof that the body runs with CPU interrupts disabled. */
static int oracle_subintr_handler(int subintr, void *arg) {
    (void)subintr;
    (void)arg;
    int idx = s_subintr_count;
    if (idx >= MUTEX_INTR_TRIALS) {
        return 0;
    }
    IntrTrial t;
    /* Interrupt-state proof: 0 when CPU interrupts are disabled here. */
    t.intr_enabled = (uint32_t)sceKernelIsCpuIntrEnable();
    /* Cell A: bad UID.  PRX must already have context-check before lookup. */
    t.r_bad_uid  = (uint32_t)sceKernelLockMutex(0x7fffffff, 1, NULL);
    /* Cell B: valid UID, bad count (0).  Context vs count ordering cell. */
    t.r_bad_cnt  = (uint32_t)sceKernelLockMutex((SceUID)s_intr_m_unlocked, 0, NULL);
    /* Cell C: valid UID, valid count, mutex unlocked.  Nominal lock-from-ISR. */
    t.r_lock     = (uint32_t)sceKernelLockMutex((SceUID)s_intr_m_unlocked, 1, NULL);
    /* Cell D: same as C but CB variant.  Context check must still fire. */
    t.r_lock_cb  = (uint32_t)sceKernelLockMutexCB((SceUID)s_intr_m_unlocked, 1, NULL);
    /* Cell E: TryLockMutex -- no blocking; no context gate documented. */
    t.r_try      = (uint32_t)sceKernelTryLockMutex((SceUID)s_intr_m_unlocked, 1);
    /* Cell F: unlock a mutex owned by the main thread -- non-owner unlock. */
    t.r_unlock   = (uint32_t)sceKernelUnlockMutex((SceUID)s_intr_m_main_owned, 1);
    s_trials[idx] = t;
    /* Barrier: write count last so main only reads a complete trial. */
    s_subintr_count = idx + 1;
    return 0;
}

/* Emit one protocol record per completed trial.
   case_id format: mutex-interrupt-context-tNN (NN = zero-padded trial index).
   out0 = intr_enabled, out1..out6 = six cell raw return values. */
static void emit_intr_trial(int emulated, int trial, const IntrTrial *t) {
    char case_id[48];
    snprintf(case_id, sizeof(case_id), "mutex-interrupt-context-t%02d", trial);
    uint32_t out[7];
    out[0] = t->intr_enabled;
    out[1] = t->r_bad_uid;
    out[2] = t->r_bad_cnt;
    out[3] = t->r_lock;
    out[4] = t->r_lock_cb;
    out[5] = t->r_try;
    out[6] = t->r_unlock;
    /* pass iff the handler ran with CPU interrupts disabled (intr_enabled == 0) */
    int pass = (t->intr_enabled == 0u);
    emit_mutex_test(emulated, case_id, pass, pass ? 1u : 0u, out, 7);
}

static uint32_t run_mutex_interrupt_context_case(int emulated) {
    s_subintr_count = 0;
    for (int i = 0; i < MUTEX_INTR_TRIALS; i++) {
        IntrTrial z = {0, 0, 0, 0, 0, 0, 0};
        s_trials[i] = z;
    }

    /* Create fixtures before enabling the interrupt so the handler always
       sees valid UIDs in s_intr_m_unlocked and s_intr_m_main_owned. */
    s_intr_m_unlocked   = sceKernelCreateMutex("oracle-intr-unl", 0, 0, NULL);
    s_intr_m_main_owned = sceKernelCreateMutex("oracle-intr-own", 0, 1, NULL);
    /* s_intr_m_main_owned initialCount=1: main thread is owner. ISR Cell F
       tests unlock from non-owner (the ISR thread context, if any). */

    int reg = sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, 0,
                                              oracle_subintr_handler, NULL);
    if (reg >= 0) {
        probe_track_subintr(PSP_VBLANK_INT, 0, sceKernelReleaseSubIntrHandler);
    }
    int ena = sceKernelEnableSubIntr(PSP_VBLANK_INT, 0);

    /* Wait for all MUTEX_INTR_TRIALS to complete.  VBLANK fires at ~60 Hz so
       20 trials need at most ~400 ms.  1000 x 1 ms is a generous timeout. */
    for (int i = 0; i < 1000 && s_subintr_count < MUTEX_INTR_TRIALS; i++) {
        sceKernelDelayThread(1000);
    }

    sceKernelDisableSubIntr(PSP_VBLANK_INT, 0);
    sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, 0);

    if (s_intr_m_unlocked >= 0)   sceKernelDeleteMutex((SceUID)s_intr_m_unlocked);
    if (s_intr_m_main_owned >= 0) sceKernelDeleteMutex((SceUID)s_intr_m_main_owned);

    /* Emit header record: reg/enable/count summary. */
    {
        uint32_t hdr[3];
        hdr[0] = (uint32_t)(reg == 0);
        hdr[1] = (uint32_t)(ena == 0);
        hdr[2] = (uint32_t)s_subintr_count;
        int pass = (reg == 0 && ena == 0 && s_subintr_count == MUTEX_INTR_TRIALS);
        emit_mutex_test(emulated, "mutex-interrupt-context", pass, pass ? 1u : 0u,
                        hdr, 3);
    }

    /* Emit per-trial records. */
    int n = s_subintr_count;
    if (n > MUTEX_INTR_TRIALS) n = MUTEX_INTR_TRIALS;
    for (int i = 0; i < n; i++) {
        emit_intr_trial(emulated, i, (const IntrTrial *)&s_trials[i]);
    }

    return (uint32_t)(reg == 0 && ena == 0 && s_subintr_count == MUTEX_INTR_TRIALS);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MBX_DELETE_WAIT
#define MBX_RECEIVE_TIMEOUT_US 500000u
#define MBX_RECEIVE_JOIN_TIMEOUT_US 1500000u
#define MBX_CONTROL_TIMEOUT_US 50000u
#define MBX_NOT_CAPTURED 0xffffffffu

/*
   Record contract:
   mbx-delete-wait result = raw DeleteMbx; out0..out25 are mailbox UID,
   receiver TID, StartThread rc, pre-delete ReferThreadStatus rc/status/
   waitType/waitId/completion count, raw ReceiveMbx rc, output message pointer,
   sentinel-unchanged bit, requested/remaining timeout, receive-entry-to-delete-
   entry elapsed, delete-entry-to-receive-return elapsed, receive-entry-to-return
   elapsed, bounded WaitThreadEnd rc, post-join completion count,
   post-join ReferThreadStatus rc/status/waitType/waitId, worker cleanup rc
   (DeleteThread after a successful join, otherwise TerminateDeleteThread),
   post-delete ReferMbxStatus rc, raw waiter count (not-captured if query fails),
   and final mailbox cleanup rc (zero means the first DeleteMbx already
   removed the object; otherwise this field records one cleanup retry).

   mbx-timeout-control result = raw ReceiveMbx; out0..out7 are its separate
   mailbox UID, output pointer, sentinel-unchanged bit, requested/remaining
   timeout, elapsed time, pre-cleanup ReferMbxStatus rc, and cleanup DeleteMbx
   rc. PASS gates only the probe's setup, blocked-state, completion, and cleanup
   integrity checks. The ReceiveMbx result and message pointers remain raw
   observations; no WAIT_DELETE return code is treated as an expected result.
*/
static volatile int s_mbx_delete_wait_uid;
static volatile uint32_t s_mbx_receive_entry_us;
static volatile uint32_t s_mbx_receive_return_us;
static volatile uint32_t s_mbx_receive_remaining_us;
static volatile uint32_t s_mbx_receive_completion_count;
static volatile int s_mbx_receive_rc;
static void * volatile s_mbx_receive_message;
static uint32_t s_mbx_message_sentinel;

static int mbx_delete_wait_receiver(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    SceUInt timeout = MBX_RECEIVE_TIMEOUT_US;
    void *message = &s_mbx_message_sentinel;
    s_mbx_receive_entry_us = sceKernelGetSystemTimeLow();
    s_mbx_receive_rc = sceKernelReceiveMbx(
        (SceUID)s_mbx_delete_wait_uid, &message, &timeout);
    s_mbx_receive_message = message;
    s_mbx_receive_remaining_us = timeout;
    s_mbx_receive_return_us = sceKernelGetSystemTimeLow();
    s_mbx_receive_completion_count++;
    return 0;
}

static uint32_t run_mbx_delete_wait(int emulated) {
    uint32_t out[26];
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
        out[i] = MBX_NOT_CAPTURED;
    }

    s_mbx_receive_entry_us = 0;
    s_mbx_receive_return_us = 0;
    s_mbx_receive_remaining_us = MBX_NOT_CAPTURED;
    s_mbx_receive_completion_count = 0;
    s_mbx_receive_rc = (int)MBX_NOT_CAPTURED;
    s_mbx_message_sentinel = 0x4d425853u;
    s_mbx_receive_message = &s_mbx_message_sentinel;

    const SceUID mbx = sceKernelCreateMbx("oracle-delete-mbx", 0, NULL);
    int delete_rc = (int)MBX_NOT_CAPTURED;
    int start_rc = (int)MBX_NOT_CAPTURED;
    int pre_status_rc = (int)MBX_NOT_CAPTURED;
    int wait_rc = (int)MBX_NOT_CAPTURED;
    int thread_cleanup_rc = (int)MBX_NOT_CAPTURED;
    uint32_t delete_entry_us = 0;
    uint32_t receive_entry_at_delete = 0;
    uint32_t receive_elapsed_us = 0;
    int receiver_started = 0;
    SceUID receiver = -1;
    uint32_t pre_status = MBX_NOT_CAPTURED;

    out[0] = (uint32_t)mbx;
    if (mbx >= 0) {
        s_mbx_delete_wait_uid = mbx;
        receiver = sceKernelCreateThread(
            "oracle-mbx-rx", mbx_delete_wait_receiver, 0x10, 0x1000,
            THREAD_ATTR_USER, NULL);
    }
    out[1] = (uint32_t)receiver;

    if (mbx >= 0 && receiver >= 0) {
        start_rc = sceKernelStartThread(receiver, 0, NULL);
        if (start_rc == 0) {
            receiver_started = 1;
            SceKernelThreadInfo before;
            memset(&before, 0, sizeof(before));
            before.size = sizeof(before);
            pre_status_rc = sceKernelReferThreadStatus(receiver, &before);
            pre_status = (uint32_t)before.status;
            out[4] = pre_status;
            out[5] = (uint32_t)before.waitType;
            out[6] = (uint32_t)before.waitId;
        }
    }
    out[2] = (uint32_t)start_rc;
    out[3] = (uint32_t)pre_status_rc;
    out[7] = s_mbx_receive_completion_count;

    if (mbx >= 0) {
        delete_entry_us = sceKernelGetSystemTimeLow();
        receive_entry_at_delete = s_mbx_receive_entry_us;
        if (receiver_started) {
            out[13] = delete_entry_us - receive_entry_at_delete;
        }
        delete_rc = sceKernelDeleteMbx(mbx);

        if (receiver_started) {
            SceUInt join_timeout = MBX_RECEIVE_JOIN_TIMEOUT_US;
            wait_rc = sceKernelWaitThreadEnd(receiver, &join_timeout);
            out[16] = (uint32_t)wait_rc;
            out[17] = s_mbx_receive_completion_count;
            out[8] = (uint32_t)s_mbx_receive_rc;
            out[9] = (uint32_t)(uintptr_t)s_mbx_receive_message;
            out[10] = (uint32_t)(
                s_mbx_receive_message == &s_mbx_message_sentinel);
            out[11] = MBX_RECEIVE_TIMEOUT_US;
            out[12] = s_mbx_receive_remaining_us;

            if (s_mbx_receive_completion_count == 1) {
                receive_elapsed_us = s_mbx_receive_return_us - receive_entry_at_delete;
                out[14] = s_mbx_receive_return_us - delete_entry_us;
                out[15] = receive_elapsed_us;
            }

            SceKernelThreadInfo after;
            memset(&after, 0, sizeof(after));
            after.size = sizeof(after);
            out[18] = (uint32_t)sceKernelReferThreadStatus(receiver, &after);
            out[19] = (uint32_t)after.status;
            out[20] = (uint32_t)after.waitType;
            out[21] = (uint32_t)after.waitId;

            if (wait_rc == 0) {
                thread_cleanup_rc = sceKernelDeleteThread(receiver);
            } else {
                thread_cleanup_rc = sceKernelTerminateDeleteThread(receiver);
            }
        } else if (receiver >= 0) {
            thread_cleanup_rc = sceKernelDeleteThread(receiver);
        }
    }

    out[22] = (uint32_t)thread_cleanup_rc;
    out[11] = MBX_RECEIVE_TIMEOUT_US;
    if (mbx >= 0) {
        SceKernelMbxInfo after_delete;
        memset(&after_delete, 0, sizeof(after_delete));
        after_delete.size = sizeof(after_delete);
        const int refer_mbx_rc = sceKernelReferMbxStatus(mbx, &after_delete);
        out[23] = (uint32_t)refer_mbx_rc;
        if (refer_mbx_rc == 0) {
            out[24] = (uint32_t)after_delete.numWaitThreads;
        }
        out[25] = (uint32_t)(delete_rc == 0 ? 0 : sceKernelDeleteMbx(mbx));
    }

    const int primary_pass =
        mbx >= 0 &&
        receiver >= 0 &&
        start_rc == 0 &&
        pre_status_rc == 0 &&
        (pre_status & PSP_THREAD_WAITING) != 0 &&
        out[5] == 5u &&
        out[6] == (uint32_t)mbx &&
        out[7] == 0u &&
        delete_rc == 0 &&
        wait_rc == 0 &&
        out[17] == 1u &&
        out[13] < MBX_RECEIVE_TIMEOUT_US &&
        out[14] < MBX_RECEIVE_TIMEOUT_US &&
        receive_elapsed_us < MBX_RECEIVE_TIMEOUT_US &&
        out[18] == 0u &&
        out[22] == 0u &&
        out[25] == 0u;
    emit_record_extended(emulated, "PSP-KERNEL-001", "mbx-delete-wait",
                         primary_pass ? "PASS" : "FAIL",
                         (uint32_t)delete_rc, out,
                         sizeof(out) / sizeof(out[0]));

    uint32_t control_out[8];
    for (size_t i = 0; i < sizeof(control_out) / sizeof(control_out[0]); i++) {
        control_out[i] = MBX_NOT_CAPTURED;
    }
    const SceUID control_mbx =
        sceKernelCreateMbx("oracle-mbx-timeout", 0, NULL);
    int control_receive_rc = (int)MBX_NOT_CAPTURED;
    int control_status_rc = (int)MBX_NOT_CAPTURED;
    int control_cleanup_rc = (int)MBX_NOT_CAPTURED;
    uint32_t control_elapsed_us = 0;
    int control_receive_called = 0;
    control_out[0] = (uint32_t)control_mbx;

    if (control_mbx >= 0) {
        uint32_t control_sentinel = 0x4d425843u;
        void *control_message = &control_sentinel;
        SceUInt control_timeout = MBX_CONTROL_TIMEOUT_US;
        const uint32_t control_start_us = sceKernelGetSystemTimeLow();
        control_receive_rc =
            sceKernelReceiveMbx(control_mbx, &control_message, &control_timeout);
        control_elapsed_us = sceKernelGetSystemTimeLow() - control_start_us;
        control_receive_called = 1;
        control_out[1] = (uint32_t)(uintptr_t)control_message;
        control_out[2] = (uint32_t)(control_message == &control_sentinel);
        control_out[3] = MBX_CONTROL_TIMEOUT_US;
        control_out[4] = control_timeout;
        control_out[5] = control_elapsed_us;

        SceKernelMbxInfo control_info;
        memset(&control_info, 0, sizeof(control_info));
        control_info.size = sizeof(control_info);
        control_status_rc =
            sceKernelReferMbxStatus(control_mbx, &control_info);
        control_cleanup_rc = sceKernelDeleteMbx(control_mbx);
        control_out[6] = (uint32_t)control_status_rc;
        control_out[7] = (uint32_t)control_cleanup_rc;
    }

    const int control_pass =
        control_mbx >= 0 &&
        control_receive_called &&
        control_status_rc == 0 &&
        control_cleanup_rc == 0;
    emit_record_extended(emulated, "PSP-KERNEL-001", "mbx-timeout-control",
                         control_pass ? "PASS" : "FAIL",
                         (uint32_t)control_receive_rc, control_out,
                         sizeof(control_out) / sizeof(control_out[0]));

    uint32_t done_out[1] = {2u};
    emit_record_extended(emulated, "PSP-KERNEL-001", "mbx-done", "PASS",
                         0, done_out, sizeof(done_out) / sizeof(done_out[0]));

    return (uint32_t)(primary_pass && control_pass);
}
#endif

static int probe_teardown_children(void) {
    int success = 1;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_threads[i].active) continue;
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(s_owned_threads[i].uid, &info) < 0) {
            s_owned_threads[i].active = 0;
            continue;
        }
        if (probe_terminate_delete_thread(s_owned_threads[i].uid) < 0) {
            success = 0;
        }
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(s_owned_threads[i].uid, &info) >= 0) {
            success = 0;
            /* The atomic helper untracks after a successful call, but a
               still-visible UID means the teardown contract was not met.
               Keep ownership live so the final verdict cannot swallow the
               residue or lose a later cleanup attempt. */
            s_owned_threads[i].active = 1;
        } else {
            s_owned_threads[i].active = 0;
        }
    }
    return success;
}

static int probe_teardown_objects(void) {
    int success = 1;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_objects[i].active) continue;
        if (probe_delete_kernel_object(s_owned_objects[i].uid,
                                       s_owned_objects[i].delete_fn) < 0) {
            success = 0;
        }
    }
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_subintr[i].active) continue;
        if (s_owned_subintr[i].release_fn(s_owned_subintr[i].intr,
                                          s_owned_subintr[i].sub) < 0) {
            success = 0;
        } else {
            s_owned_subintr[i].active = 0;
        }
    }
    return success;
}

static int probe_teardown_io_audio(void) {
    int success = 1;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_audio[i].active) continue;
        if (s_owned_audio[i].release_fn(s_owned_audio[i].channel) < 0) {
            success = 0;
        } else {
            s_owned_audio[i].active = 0;
        }
    }
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_fds[i].active) continue;
        if (probe_io_close(s_owned_fds[i].uid) < 0) {
            success = 0;
        }
    }
    return success;
}

static int probe_teardown_memory(void) {
    int success = 1;
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (!s_owned_memory[i].active) continue;
        if (probe_free_partition_memory(s_owned_memory[i].uid) < 0) {
            success = 0;
        }
    }
    return success;
}

static int probe_teardown_state(void) {
    int success = 1;
    if (s_have_clock_state) {
        if (scePowerGetCpuClockFrequencyInt() != s_boot_cpu_mhz) {
            if (scePowerSetCpuClockFrequency(s_boot_cpu_mhz) < 0 ||
                scePowerGetCpuClockFrequencyInt() != s_boot_cpu_mhz) {
                success = 0;
            }
        }
        if (scePowerGetBusClockFrequencyInt() != s_boot_bus_mhz) {
            if (scePowerSetBusClockFrequency(s_boot_bus_mhz) < 0 ||
                scePowerGetBusClockFrequencyInt() != s_boot_bus_mhz) {
                success = 0;
            }
        }
    } else {
        success = 0;
    }
#if defined(__mips__)
    if (s_have_fcr31_state) {
        uint32_t current_fcr31 = 0;
        __asm__ volatile("cfc1 %0, $31" : "=r"(current_fcr31) :: "memory");
        if (current_fcr31 != s_boot_fcr31) {
            __asm__ volatile("ctc1 %0, $31" :: "r"(s_boot_fcr31) : "memory");
            __asm__ volatile("cfc1 %0, $31" : "=r"(current_fcr31) :: "memory");
            if (current_fcr31 != s_boot_fcr31) success = 0;
        }
    } else {
        success = 0;
    }
#endif
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; i++) {
        if (s_pending_intr_active[i]) {
            if (probe_resume_intr(s_pending_intr_tokens[i]) < 0) success = 0;
        }
    }
    return success;
}

static int probe_host0_roundtrip(void) {
    uint8_t expected[64];
    uint8_t observed[64];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(observed, 0, sizeof(observed));
    SceUID fd = sceIoOpen(PROBE_HOST0_ROUNDTRIP_PATH,
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return 0;
    const int written = sceIoWrite(fd, expected, (SceSize)sizeof(expected));
    const int write_close = sceIoClose(fd);
    if (written != (int)sizeof(expected) || write_close < 0) return 0;

    fd = sceIoOpen(PROBE_HOST0_ROUNDTRIP_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    const int read = sceIoRead(fd, observed, (SceSize)sizeof(observed));
    const int read_close = sceIoClose(fd);
    return read == (int)sizeof(observed) && read_close >= 0 &&
           memcmp(expected, observed, sizeof(expected)) == 0;
}

static int probe_teardown_host0(int emulated) {
    if (emulated) return PROBE_TEARDOWN_SKIPPED;
    int success = 1;
    for (size_t i = 0; i < PROBE_TEARDOWN_PATH_CAPACITY; i++) {
        if (!s_owned_paths[i].active) continue;
        if (probe_io_remove(s_owned_paths[i].path) < 0) {
            success = 0;
        } else {
            s_owned_paths[i].active = 0;
        }
    }
    return probe_host0_roundtrip() && success
               ? PROBE_TEARDOWN_PASSED
               : PROBE_TEARDOWN_FAILED;
}

/* Module lifecycle for repeated launches. PSPSDK's PRX CRT (crt0_prx) creates
   the main thread in module_start and exports no module_stop. Its only path
   that ends main is exit() -> _exit(), which runs _fini and __libcglue_deinit
   and then calls sceKernelExitGame(). PSPLink hooks that call: it resets when
   resetonexit=1 (its default) and otherwise exits the calling thread without
   deleting it. Stopping and unloading a module neither ends nor deletes the
   threads it created. A probe that parks main and is then stopped and
   unloaded by PSPLink therefore leaves main (and the newlib heap) behind.

   The probe owns the stop half of its lifecycle. module_stop asks the parked
   main thread to end. Main runs the CRT's runtime de-initialisation (the
   _exit steps before sceKernelExitGame, which free the newlib heap and the
   C runtime's kernel objects), then exits. module_stop waits a bounded time
   for that end, deletes main, and returns 0. When main does not end within
   the bound, module_stop returns 1 ("cannot stop") instead of removing a
   thread that may still be running: the module stays loaded, and the host's
   unload and snapshot checks report the failure. Main never deletes itself;
   see docs/HARDWARE_ORACLE.md, "Probe teardown for repeated launches". */
extern void _fini(void);
extern void __libcglue_deinit(void);

#define PROBE_MAIN_STOP_TIMEOUT_US 1000000u

static volatile SceUID s_probe_main_thread = -1;
static volatile int s_probe_stop_requested;

static void probe_park_until_stop(void) {
    /* A wakeup that arrives before the sleep is counted, not lost. */
    while (!s_probe_stop_requested) sceKernelSleepThread();
    _fini();
    __libcglue_deinit();
    sceKernelExitThread(0);
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const SceUID main_thread = s_probe_main_thread;
    if (main_thread < 0) return 1;
    s_probe_stop_requested = 1;
    (void)sceKernelWakeupThread(main_thread); /* fails harmlessly if main already ended */
    SceUInt timeout = PROBE_MAIN_STOP_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(main_thread, &timeout) < 0) return 1;
    if (sceKernelDeleteThread(main_thread) < 0) return 1;
    s_probe_main_thread = -1;
    return 0;
}

static void probe_teardown(int emulated) {
    sceKernelDcacheWritebackAll();
    int success = !s_teardown_tracking_failed && s_have_clock_state;
    if (!probe_teardown_children()) success = 0;
    if (!probe_teardown_objects()) success = 0;
    if (!probe_teardown_io_audio()) success = 0;
    if (!probe_teardown_memory()) success = 0;
    if (!probe_teardown_state()) success = 0;
    const int host0_result = probe_teardown_host0(emulated);
    if (host0_result == PROBE_TEARDOWN_FAILED) success = 0;

#ifdef PROBE_HOST0_LOG
    SceUID log_fd = -1;
    if (!emulated) {
        log_fd = sceIoOpen(PROBE_HOST0_LOG, PSP_O_WRONLY | PSP_O_APPEND, 0777);
        if (log_fd < 0) success = 0;
    }
#endif
    char sentinel[80];
    const char *status = !success ? "FAIL"
                         : host0_result == PROBE_TEARDOWN_SKIPPED ? "NOT_RUN"
                         : "PASS";
    snprintf(sentinel, sizeof(sentinel),
             "NAKAGAWA_PSP_COMPLETE schema=1 status=%s\n",
             status);
#ifdef PROBE_HOST0_LOG
    if (log_fd >= 0) {
        const int wrote = sceIoWrite(log_fd, sentinel, (SceSize)strlen(sentinel));
        (void)sceIoClose(log_fd);
        if (wrote != (int)strlen(sentinel)) {
            success = 0;
            snprintf(sentinel, sizeof(sentinel),
                     "NAKAGAWA_PSP_COMPLETE schema=1 status=FAIL\n");
        }
    }
#endif
    emit(emulated, sentinel);
    probe_park_until_stop();
}

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_KERNEL_ALARM
/* Upper bound for the alarm-table exhaustion cell.  The previous revision set
   alarms until the kernel refused one, with no bound.  1024 pending alarms is
   far beyond what any game keeps at once. */
#define ALARM_EXHAUSTION_CAP 1024u
static SceUID s_alarm_table[ALARM_EXHAUSTION_CAP];
static volatile uint32_t s_alarm_fired;
static volatile uint32_t s_alarm_first_us;
static volatile uint32_t s_alarm_second_us;
static volatile uint32_t s_alarm_handler_us;
static volatile uint32_t s_alarm_spin_sink;
static SceUID s_alarm_blocking_sema;
static volatile int s_alarm_blocking_rc;
static volatile int s_alarm_intr_enabled;

#define ALARM_REARM_DELAY_US 50000u
#define ALARM_HANDLER_WORK_US 100000u

static SceUInt alarm_count_handler(void *common) {
    volatile uint32_t *count = (volatile uint32_t *)common;
    (*count)++;
    return 0;
}

static SceUInt alarm_rearm_handler(void *common) {
    (void)common;
    uint32_t current = ++s_alarm_fired;
    uint32_t now = sceKernelGetSystemTimeLow();
    if (current == 1u) {
        s_alarm_first_us = now;
        while ((uint32_t)(sceKernelGetSystemTimeLow() - now) <
               ALARM_HANDLER_WORK_US) {
            s_alarm_spin_sink++;
        }
        s_alarm_handler_us = (uint32_t)(sceKernelGetSystemTimeLow() - now);
        return ALARM_REARM_DELAY_US;
    } else {
        s_alarm_second_us = now;
    }
    return 0;
}

static SceUInt alarm_blocking_handler(void *common) {
    (void)common;
    s_alarm_intr_enabled = sceKernelIsCpuIntrEnable();
    SceUInt timeout = 1000u;
    s_alarm_blocking_rc = sceKernelWaitSema(s_alarm_blocking_sema, 1, &timeout);
    s_alarm_fired++;
    return 0;
}

static int wait_for_alarm_count(volatile uint32_t *count, uint32_t wanted,
                                uint32_t timeout_us) {
    SceInt64 start = sceKernelGetSystemTimeWide();
    while (*count < wanted && sceKernelGetSystemTimeWide() - start < timeout_us) {
        sceKernelDelayThread(1000u);
    }
    return *count >= wanted;
}

static void emit_alarm_case(int emulated, const char *case_id, uint32_t result,
                            const uint32_t *out, size_t count) {
    emit_record_extended(emulated, "PSP-ALARM-001", case_id, "PASS", result,
                         out, count);
}

static void run_kernel_alarm(int emulated) {
    uint32_t out[5];
    SceUID uid = sceKernelSetAlarm(1000000u, NULL, NULL);
    out[0] = (uint32_t)uid;
    out[1] = uid >= 0 ? (uint32_t)sceKernelCancelAlarm(uid) : 0xffffffffu;
    emit_alarm_case(emulated, "alarm-null-handler", (uint32_t)uid, out, 2);

    s_alarm_fired = 0;
    uid = sceKernelSetAlarm(0u, alarm_count_handler, (void *)&s_alarm_fired);
    out[0] = (uint32_t)uid;
    out[1] = wait_for_alarm_count(&s_alarm_fired, 1u, 500000u);
    out[2] = (uint32_t)s_alarm_fired;
    emit_alarm_case(emulated, "alarm-zero-clock", (uint32_t)uid, out, 3);

    /* Bounded table exhaustion: set pending alarms until sceKernelSetAlarm
       fails or ALARM_EXHAUSTION_CAP alarms are pending.  out0 counts the
       alarms set, out1 is the first failure (0 when none failed), out2 is the
       cap, so out0 == out2 with out1 == 0 is the measured outcome "no
       exhaustion within the cap". */
    uint32_t alarm_count = 0;
    int first_alarm_error = 0;
    probe_step(emulated, "kernel-alarm", "alarm-table-exhaustion");
    while (alarm_count < ALARM_EXHAUSTION_CAP) {
        SceUID alarm = sceKernelSetAlarm(0x7fffffffu, alarm_count_handler,
                                         (void *)&s_alarm_fired);
        if (alarm < 0) {
            first_alarm_error = alarm;
            break;
        }
        s_alarm_table[alarm_count++] = alarm;
    }
    probe_step(emulated, "kernel-alarm", "alarm-table-release");
    for (uint32_t i = 0; i < alarm_count; i++) {
        (void)sceKernelCancelAlarm(s_alarm_table[i]);
    }
    out[0] = alarm_count;
    out[1] = (uint32_t)first_alarm_error;
    out[2] = ALARM_EXHAUSTION_CAP;
    emit_alarm_case(emulated, "alarm-table-exhaustion", (uint32_t)first_alarm_error,
                    out, 3);

    s_alarm_fired = 0;
    uid = sceKernelSetAlarm(1000u, alarm_count_handler, (void *)&s_alarm_fired);
    const uint32_t fired = (uint32_t)wait_for_alarm_count(&s_alarm_fired, 1u, 500000u);
    const int fired_cancel = uid >= 0 ? sceKernelCancelAlarm(uid) : uid;
    out[0] = (uint32_t)uid;
    out[1] = fired;
    out[2] = (uint32_t)fired_cancel;
    emit_alarm_case(emulated, "alarm-cancel-fired-once", (uint32_t)fired_cancel, out, 3);

    uid = sceKernelSetAlarm(1000000u, alarm_count_handler, (void *)&s_alarm_fired);
    const int cancel_first = uid >= 0 ? sceKernelCancelAlarm(uid) : uid;
    const int cancel_second = uid >= 0 ? sceKernelCancelAlarm(uid) : uid;
    out[0] = (uint32_t)uid;
    out[1] = (uint32_t)cancel_first;
    out[2] = (uint32_t)cancel_second;
    emit_alarm_case(emulated, "alarm-cancel-cancelled", (uint32_t)cancel_first, out, 3);

    const int unknown_cancel = sceKernelCancelAlarm((SceUID)-1);
    out[0] = (uint32_t)unknown_cancel;
    emit_alarm_case(emulated, "alarm-cancel-unknown", (uint32_t)unknown_cancel, out, 1);

    s_alarm_fired = 0;
    s_alarm_first_us = 0;
    s_alarm_second_us = 0;
    s_alarm_handler_us = 0;
    s_alarm_spin_sink = 0;
    uid = sceKernelSetAlarm(20000u, alarm_rearm_handler, NULL);
    const uint32_t rearm_complete = (uint32_t)wait_for_alarm_count(&s_alarm_fired, 2u, 750000u);
    out[0] = (uint32_t)uid;
    out[1] = s_alarm_fired;
    out[2] = s_alarm_handler_us;
    out[3] = rearm_complete ? s_alarm_second_us - s_alarm_first_us : 0xffffffffu;
    out[4] = ALARM_REARM_DELAY_US;
    if (uid >= 0) (void)sceKernelCancelAlarm(uid);
    emit_alarm_case(emulated, "alarm-rearm-base", (uint32_t)uid, out, 5);

    s_alarm_fired = 0;
    s_alarm_blocking_rc = (int)0xdeadbeefu;
    s_alarm_intr_enabled = -1;
    s_alarm_blocking_sema = sceKernelCreateSema("oracle-alarm-block", 0, 0, 1, NULL);
    uid = sceKernelSetAlarm(1000u, alarm_blocking_handler, NULL);
    const uint32_t blocking_complete = (uint32_t)wait_for_alarm_count(&s_alarm_fired, 1u, 500000u);
    if (uid >= 0) (void)sceKernelCancelAlarm(uid);
    out[0] = (uint32_t)uid;
    out[1] = (uint32_t)s_alarm_intr_enabled;
    out[2] = (uint32_t)s_alarm_blocking_rc;
    out[3] = (uint32_t)s_alarm_fired;
    out[4] = blocking_complete;
    emit_alarm_case(emulated, "alarm-blocking-in-handler", (uint32_t)s_alarm_blocking_rc,
                    out, 5);
    uint32_t done = 8;
    emit_record_extended(emulated, "PSP-ALARM-001", "kernel-alarm-done", "PASS",
                         0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_SCHEDULER
static SceUID s_thread_control_sema;
static SceUID s_thread_wait_sema;
static SceUID s_thread_done_sema;
static SceUID s_thread_suspend_target;
static volatile int s_thread_action_rc;
static volatile int s_thread_wait_rc;
static volatile uint32_t s_thread_ready_order[3];
static volatile uint32_t s_thread_ready_count;
static uint32_t s_thread_order_args[3];

static int thread_self_suspend_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    (void)sceKernelSignalSema(s_thread_control_sema, 1);
    SceUID target = s_thread_suspend_target == 0 ? 0 : sceKernelGetThreadId();
    s_thread_action_rc = sceKernelSuspendThread(target);
    (void)sceKernelSignalSema(s_thread_done_sema, 1);
    return 0;
}

static SceUID s_thread_order_sema;

static int thread_order_entry(SceSize args, void *argp) {
    (void)args;
    uint32_t index = s_thread_ready_count++;
    if (index < 3u) s_thread_ready_order[index] = *(const uint32_t *)argp;
    if (s_thread_ready_count == 3u) (void)sceKernelSignalSema(s_thread_order_sema, 1);
    return 0;
}

static int thread_timed_wait_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 50000u;
    s_thread_wait_rc = sceKernelWaitSema(s_thread_wait_sema, 1, &timeout);
    (void)sceKernelSignalSema(s_thread_done_sema, 1);
    return 0;
}

static int wait_for_thread_state(SceUID uid, int desired, uint32_t timeout_us,
                                 uint32_t *observed) {
    SceInt64 started = sceKernelGetSystemTimeWide();
    do {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(uid, &info) == 0) {
            *observed = info.status;
            if (info.status == desired) return 1;
        }
        sceKernelDelayThread(1000u);
    } while (sceKernelGetSystemTimeWide() - started < timeout_us);
    return 0;
}

static void emit_thread_scheduler(int emulated, const char *case_id,
                                  uint32_t result, const uint32_t *out,
                                  size_t count) {
    emit_record_extended(emulated, "PSP-THREAD-003", case_id, "PASS", result,
                         out, count);
}

static void run_suspend_target(int emulated, const char *case_id, SceUID target) {
    s_thread_control_sema = sceKernelCreateSema("oracle-suspend-enter", 0, 0, 1, NULL);
    s_thread_done_sema = sceKernelCreateSema("oracle-suspend-done", 0, 0, 1, NULL);
    s_thread_suspend_target = target;
    s_thread_action_rc = (int)0xdeadbeefu;
    SceUID thread = sceKernelCreateThread("oracle-suspend-target",
        thread_self_suspend_entry, 64, 0x1000, THREAD_ATTR_USER, NULL);
    int start_rc = thread >= 0 ? sceKernelStartThread(thread, 0, NULL) : thread;
    probe_step(emulated, "thread-scheduler", case_id);
    SceUInt control_timeout = 1000000u;
    if (start_rc >= 0) (void)sceKernelWaitSema(s_thread_control_sema, 1, &control_timeout);
    uint32_t observed = 0xffffffffu;
    uint32_t suspended = thread >= 0 ?
        (uint32_t)wait_for_thread_state(thread, PSP_THREAD_SUSPEND, 100000u,
                                       &observed) : 0u;
    int resume_rc = thread >= 0 ? sceKernelResumeThread(thread) : thread;
    SceUInt done_timeout = 1000000u;
    int done_rc = thread >= 0 ?
        sceKernelWaitSema(s_thread_done_sema, 1, &done_timeout) : thread;
    uint32_t out[4] = {
        (uint32_t)start_rc, suspended, (uint32_t)resume_rc,
        (uint32_t)s_thread_action_rc,
    };
    emit_thread_scheduler(emulated, case_id, (uint32_t)done_rc, out, 4);
    if (thread >= 0 && done_rc >= 0) {
        SceUInt join_timeout = 1000000u;
        (void)sceKernelWaitThreadEnd(thread, &join_timeout);
        (void)sceKernelDeleteThread(thread);
    }
}

static void run_thread_scheduler(int emulated) {
    uint32_t out[7] = {0};
    run_suspend_target(emulated, "thread-suspend-idle", 0);
    run_suspend_target(emulated, "thread-suspend-self", -1);

    int rc = sceKernelResumeThread(0);
    out[0] = (uint32_t)rc;
    emit_thread_scheduler(emulated, "thread-resume-idle", (uint32_t)rc, out, 1);

    rc = sceKernelRotateThreadReadyQueue(0xff);
    out[0] = 0xffu;
    emit_thread_scheduler(emulated, "thread-rotate-range", (uint32_t)rc, out, 1);

    s_thread_order_sema = sceKernelCreateSema("oracle-ready-done", 0, 0, 1, NULL);
    s_thread_ready_count = 0;
    s_thread_ready_order[0] = 0xffffffffu;
    s_thread_ready_order[1] = 0xffffffffu;
    s_thread_ready_order[2] = 0xffffffffu;
    s_thread_order_args[0] = 0u;
    s_thread_order_args[1] = 1u;
    s_thread_order_args[2] = 2u;
    SceUID order_a = sceKernelCreateThread("oracle-ready-a", thread_order_entry,
        64, 0x1000, THREAD_ATTR_USER, NULL);
    SceUID order_b = sceKernelCreateThread("oracle-ready-b", thread_order_entry,
        64, 0x1000, THREAD_ATTR_USER, NULL);
    SceUID order_c = sceKernelCreateThread("oracle-ready-c", thread_order_entry,
        64, 0x1000, THREAD_ATTR_USER, NULL);
    int start_a = order_a >= 0 ? sceKernelStartThread(
        order_a, sizeof(s_thread_order_args[0]), &s_thread_order_args[0]) : order_a;
    int start_b = order_b >= 0 ? sceKernelStartThread(
        order_b, sizeof(s_thread_order_args[1]), &s_thread_order_args[1]) : order_b;
    int rotate_rc = sceKernelRotateThreadReadyQueue(64);
    int start_c = order_c >= 0 ? sceKernelStartThread(
        order_c, sizeof(s_thread_order_args[2]), &s_thread_order_args[2]) : order_c;
    /* Block (bounded) until the third ready thread has run.  The previous
       revision slept with no bound; a lost wakeup would have hung the case.
       A timeout is recorded as TIMEOUT with the order observed so far. */
    probe_step(emulated, "thread-scheduler", "thread-ready-order-after-rotate");
    SceUInt order_timeout = 1000000u;
    const int order_rc = order_a >= 0 && order_b >= 0 && order_c >= 0 ?
        sceKernelWaitSema(s_thread_order_sema, 1, &order_timeout) : -1;
    out[0] = (uint32_t)start_a;
    out[1] = (uint32_t)start_b;
    out[2] = (uint32_t)rotate_rc;
    out[3] = (uint32_t)start_c;
    out[4] = s_thread_ready_order[0];
    out[5] = s_thread_ready_order[1];
    out[6] = s_thread_ready_order[2];
    emit_record_extended(emulated, "PSP-THREAD-003", "thread-ready-order-after-rotate",
                         order_rc == 0 ? "PASS" : "TIMEOUT", (uint32_t)rotate_rc,
                         out, 7);
    if (order_a >= 0) {
        SceUInt timeout = 1000000u;
        (void)sceKernelWaitThreadEnd(order_a, &timeout);
        (void)sceKernelDeleteThread(order_a);
    }
    if (order_b >= 0) {
        SceUInt timeout = 1000000u;
        (void)sceKernelWaitThreadEnd(order_b, &timeout);
        (void)sceKernelDeleteThread(order_b);
    }
    if (order_c >= 0) {
        SceUInt timeout = 1000000u;
        (void)sceKernelWaitThreadEnd(order_c, &timeout);
        (void)sceKernelDeleteThread(order_c);
    }

    s_thread_wait_sema = sceKernelCreateSema("oracle-suspend-wait", 0, 0, 1, NULL);
    s_thread_done_sema = sceKernelCreateSema("oracle-suspend-wait-done", 0, 0, 1, NULL);
    s_thread_wait_rc = (int)0xdeadbeefu;
    SceUID waiting = sceKernelCreateThread("oracle-suspend-wait", thread_timed_wait_entry,
        64, 0x1000, THREAD_ATTR_USER, NULL);
    int waiting_start = waiting >= 0 ? sceKernelStartThread(waiting, 0, NULL) : waiting;
    uint32_t waiting_state = 0xffffffffu;
    uint32_t entered_wait = waiting >= 0 ?
        (uint32_t)wait_for_thread_state(waiting, PSP_THREAD_WAITING, 100000u,
                                       &waiting_state) : 0u;
    int suspend_rc = waiting >= 0 ? sceKernelSuspendThread(waiting) : waiting;
    sceKernelDelayThread(100000u);
    int resume_rc = waiting >= 0 ? sceKernelResumeThread(waiting) : waiting;
    SceUInt done_timeout = 1000000u;
    int wait_done = waiting >= 0 ?
        sceKernelWaitSema(s_thread_done_sema, 1, &done_timeout) : waiting;
    out[0] = (uint32_t)waiting_start;
    out[1] = entered_wait;
    out[2] = (uint32_t)suspend_rc;
    out[3] = (uint32_t)resume_rc;
    out[4] = (uint32_t)s_thread_wait_rc;
    emit_thread_scheduler(emulated, "thread-suspend-wait-timeout",
                          (uint32_t)wait_done, out, 5);
    if (waiting >= 0 && wait_done >= 0) {
        SceUInt timeout = 1000000u;
        (void)sceKernelWaitThreadEnd(waiting, &timeout);
        (void)sceKernelDeleteThread(waiting);
    }

    uint32_t done = 6;
    emit_record_extended(emulated, "PSP-THREAD-003", "thread-scheduler-done",
                         "PASS", 0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_OUTCOMES
int sceKernelCancelSema(SceUID semaid, int newCount, int *numWaitThreads);
int sceKernelCancelEventFlag(SceUID evid, SceUInt newPattern,
                             int *numWaitThreads);

static int s_wait_mode;
static int s_wait_result;
static SceUID s_wait_object;
static SceUID s_wait_done_sema;
static volatile SceInt64 s_wait_entered;

static int wait_outcomes_wait_thread_state(SceUID uid, int desired,
                                          uint32_t timeout_us) {
    SceInt64 started = sceKernelGetSystemTimeWide();
    do {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(uid, &info) == 0 &&
            info.status == desired) return 1;
        sceKernelDelayThread(1000u);
    } while (sceKernelGetSystemTimeWide() - started < timeout_us);
    return 0;
}

static int wait_outcomes_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 50000u;
    s_wait_entered = sceKernelGetSystemTimeWide();
    if (s_wait_mode == 0) {
        s_wait_result = sceKernelWaitSema(s_wait_object, 1, &timeout);
    } else {
        s_wait_result = sceKernelWaitEventFlag(s_wait_object, 1u,
            PSP_EVENT_WAITOR, NULL, &timeout);
    }
    (void)sceKernelSignalSema(s_wait_done_sema, 1);
    return 0;
}

static void run_wait_outcome(int emulated, const char *case_id,
                             int mode, int cancel) {
    probe_step(emulated, "wait-outcomes", case_id);
    s_wait_mode = mode;
    s_wait_result = (int)0xdeadbeefu;
    s_wait_entered = 0;
    s_wait_object = mode == 0 ?
        sceKernelCreateSema("oracle-late-sema", 0, 0, 1, NULL) :
        sceKernelCreateEventFlag("oracle-late-event", 0, 0, NULL);
    s_wait_done_sema = sceKernelCreateSema("oracle-late-done", 0, 0, 1, NULL);
    SceUID thread = sceKernelCreateThread("oracle-late-wait", wait_outcomes_entry,
        64, 0x1000, THREAD_ATTR_USER, NULL);
    int start_rc = thread >= 0 ? sceKernelStartThread(thread, 0, NULL) : thread;
    uint32_t waiting = thread >= 0 ?
        (uint32_t)wait_outcomes_wait_thread_state(thread, PSP_THREAD_WAITING,
                                                 100000u) : 0u;
    int action_rc = s_wait_object;
    if (waiting && mode == 0 && cancel) {
        action_rc = sceKernelCancelSema(s_wait_object, 0, NULL);
    } else if (waiting && mode == 0) {
        action_rc = sceKernelSignalSema(s_wait_object, 1);
    } else if (waiting && cancel) {
        action_rc = sceKernelCancelEventFlag(s_wait_object, 0, NULL);
    } else if (waiting) {
        action_rc = sceKernelSetEventFlag(s_wait_object, 1u);
    }
    SceInt64 deadline = s_wait_entered + 75000;
    while (s_wait_entered != 0 && sceKernelGetSystemTimeWide() < deadline) {
        /* Keep the higher-priority controller runnable until after the wait deadline. */
    }
    SceInt64 dispatched = sceKernelGetSystemTimeWide();
    SceUInt done_timeout = 1000000u;
    int done_rc = thread >= 0 ?
        sceKernelWaitSema(s_wait_done_sema, 1, &done_timeout) : thread;
    uint32_t out[5] = {
        (uint32_t)start_rc,
        waiting,
        (uint32_t)action_rc,
        (uint32_t)s_wait_result,
        (uint32_t)(waiting && dispatched >= deadline),
    };
    emit_record_extended(emulated, "PSP-WAIT-001", case_id,
                         waiting && done_rc >= 0 ? "PASS" : "SKIP",
                         (uint32_t)action_rc, out, 5);
    if (thread >= 0 && done_rc >= 0) {
        SceUInt join_timeout = 1000000u;
        (void)sceKernelWaitThreadEnd(thread, &join_timeout);
        (void)sceKernelDeleteThread(thread);
    }
}

static void run_wait_outcomes(int emulated) {
    run_wait_outcome(emulated, "sema-signal-before-deadline-late-dispatch", 0, 0);
    run_wait_outcome(emulated, "event-signal-before-deadline-late-dispatch", 1, 0);
    run_wait_outcome(emulated, "sema-cancel-before-deadline-late-dispatch", 0, 1);
    run_wait_outcome(emulated, "event-cancel-before-deadline-late-dispatch", 1, 1);
    uint32_t done = 4;
    emit_record_extended(emulated, "PSP-WAIT-001", "wait-outcomes-done", "PASS",
                         0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_REFER_STATUS_SIZE
typedef int (*ReferStatusFn)(SceUID, void *);

static int refer_sema_status(SceUID uid, void *info) {
    return sceKernelReferSemaStatus(uid, (SceKernelSemaInfo *)info);
}

static int refer_event_status(SceUID uid, void *info) {
    return sceKernelReferEventFlagStatus(uid, (SceKernelEventFlagInfo *)info);
}

static int refer_mbx_status(SceUID uid, void *info) {
    return sceKernelReferMbxStatus(uid, (SceKernelMbxInfo *)info);
}

static void run_status_size_cell(int emulated, const char *case_id, SceUID uid,
                                 size_t full_size, ReferStatusFn refer,
                                 uint32_t requested) {
    uint8_t bytes[64];
    uint8_t before[64];
    memset(bytes, 0xa5, sizeof(bytes));
    *(SceSize *)bytes = (SceSize)requested;
    memcpy(before, bytes, sizeof(bytes));
    probe_step(emulated, "refer-status-size", case_id);
    int rc = refer(uid, bytes);
    uint32_t low_mask = 0;
    uint32_t high_mask = 0;
    uint32_t changed = 0;
    for (uint32_t i = 0; i < sizeof(bytes); i++) {
        if (bytes[i] != before[i]) {
            if (i < 32u) low_mask |= 1u << i;
            else high_mask |= 1u << (i - 32u);
            changed++;
        }
    }
    uint32_t out[5] = {
        requested,
        *(SceSize *)bytes,
        low_mask,
        high_mask,
        changed,
    };
    const char *status = full_size <= sizeof(bytes) ? "PASS" : "FAIL";
    emit_record_extended(emulated, "PSP-KERNEL-STATUS-001", case_id,
                         status, (uint32_t)rc, out, 5);
}

static void run_refer_status_size(int emulated) {
    SceUID sema = sceKernelCreateSema("oracle-size-sema", 0, 1, 2, NULL);
    SceUID event = sceKernelCreateEventFlag("oracle-size-event", 0, 0x12u, NULL);
    SceUID mbx = sceKernelCreateMbx("oracle-size-mbx", 0, NULL);
    const uint32_t requests[] = {0u, 8u, 40u, 0xffffffffu};
    const char *labels[] = {"zero", "8", "40", "full"};
    char case_id[48];
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        uint32_t size = requests[i] == 0xffffffffu ? sizeof(SceKernelSemaInfo) : requests[i];
        snprintf(case_id, sizeof(case_id), "sema-size-%s", labels[i]);
        run_status_size_cell(emulated, case_id, sema, sizeof(SceKernelSemaInfo),
                             refer_sema_status, size);
    }
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        uint32_t size = requests[i] == 0xffffffffu ? sizeof(SceKernelEventFlagInfo) : requests[i];
        snprintf(case_id, sizeof(case_id), "event-size-%s", labels[i]);
        run_status_size_cell(emulated, case_id, event, sizeof(SceKernelEventFlagInfo),
                             refer_event_status, size);
    }
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        uint32_t size = requests[i] == 0xffffffffu ? sizeof(SceKernelMbxInfo) : requests[i];
        snprintf(case_id, sizeof(case_id), "mbx-size-%s", labels[i]);
        run_status_size_cell(emulated, case_id, mbx, sizeof(SceKernelMbxInfo),
                             refer_mbx_status, size);
    }
    uint32_t done = 12;
    emit_record_extended(emulated, "PSP-KERNEL-STATUS-001", "refer-status-size-done",
                         "PASS", 0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_READONLY
static uint32_t s_registry_categories;
static uint32_t s_registry_keys;
static uint32_t s_registry_records;

static int registry_name_is_modeled(const char *name) {
    static const char *const modeled[] = {
        "language", "button_assign", "date_format", "time_format",
        "timezone", "summer_time", "adhoc_channel",
    };
    for (size_t i = 0; i < sizeof(modeled) / sizeof(modeled[0]); i++) {
        if (strcmp(name, modeled[i]) == 0) return 1;
    }
    return 0;
}

static int registry_name_is_safe(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;
    for (size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static void emit_registry_record(int emulated, const char *case_id,
                                 const char *status, uint32_t result,
                                 const uint32_t *out, size_t out_count,
                                 const char *detail, const uint8_t *value,
                                 size_t value_bytes) {
    if (detail == NULL && value == NULL && value_bytes == 0u) {
        emit_record_extended(emulated, "PSP-REGISTRY-001", case_id, status,
                             result, out, out_count);
        /* registry-done out2 counts every record before it, fixed or census. */
        s_registry_records++;
        return;
    }
    size_t capacity = 512u;
    if (detail != NULL && strlen(detail) <= (size_t)-1 - capacity) {
        capacity += strlen(detail);
    }
    if (value != NULL && value_bytes <= ((size_t)-1 - capacity) / 2u) {
        capacity += value_bytes * 2u;
    } else if (value_bytes != 0u) {
        return;
    }
    char *line = (char *)malloc(capacity);
    if (line == NULL) return;
    int used = snprintf(line, capacity,
        "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 case_id=%s "
        "status=%s result=0x%08x", case_id, status, (unsigned int)result);
    for (size_t i = 0; i < out_count && used > 0 && (size_t)used < capacity; i++) {
        int wrote = snprintf(line + used, capacity - (size_t)used,
                             " out%u=0x%08x", (unsigned int)i,
                             (unsigned int)out[i]);
        if (wrote < 0) break;
        used += wrote;
    }
    if (detail != NULL && used > 0 && (size_t)used < capacity) {
        int wrote = snprintf(line + used, capacity - (size_t)used,
                             " detail=%s", detail);
        if (wrote < 0) used = -1;
        else used += wrote;
    }
    if (value != NULL && used > 0 && (size_t)used < capacity) {
        int wrote = snprintf(line + used, capacity - (size_t)used, " value_hex=");
        if (wrote < 0) used = -1;
        else used += wrote;
        for (size_t i = 0; i < value_bytes && used > 0 && (size_t)used + 2u < capacity; i++) {
            int count = snprintf(line + used, capacity - (size_t)used, "%02x", value[i]);
            if (count != 2) { used = -1; break; }
            used += count;
        }
    }
    if (used > 0 && (size_t)used + 1u < capacity) {
        line[used++] = '\n';
        line[used] = '\0';
        probe_emit_durable(emulated, line, (size_t)used);
        s_registry_records++;
    }
    free(line);
}

static int registry_read_key(int emulated, REGHANDLE category, const char *path,
                             const char *name, uint32_t index) {
    REGHANDLE key = 0;
    unsigned int type = 0;
    SceSize size = 0;
    int info_rc = sceRegGetKeyInfo(category, name, &key, &type, &size);
    uint32_t out[4] = {
        (uint32_t)type, (uint32_t)size, 0u, 0xffffffffu,
    };
    uint8_t *value = NULL;
    size_t value_bytes = 0;
    int modeled = registry_name_is_modeled(name) && type != REG_TYPE_DIR;
    int safe_size = size > 0u && (size_t)size <= (size_t)-1 / 2u;
    int value_rc = (int)0xffffffffu;
    if (info_rc == 0 && modeled && safe_size) {
        value = (uint8_t *)malloc((size_t)size);
        if (value != NULL) {
            value_rc = sceRegGetKeyValue(category, key, value, size);
            out[3] = (uint32_t)value_rc;
            if (value_rc == 0) {
                out[2] = 1u;
                value_bytes = size;
            }
        }
    }
    char record_id[48];
    char detail[512];
    snprintf(record_id, sizeof(record_id), "registry-key-%04u",
             (unsigned int)index);
    snprintf(detail, sizeof(detail), "%s/%s", path, name);
    emit_registry_record(emulated, record_id, "PASS", (uint32_t)info_rc,
                         out, 4, detail, value_bytes ? value : NULL, value_bytes);
    free(value);
    s_registry_keys++;
    return type == REG_TYPE_DIR;
}

static int registry_probe_small_buffer(REGHANDLE category) {
    int count = -1;
    if (sceRegGetKeysNum(category, &count) != 0 || count <= 0 ||
        (size_t)count > (size_t)-1 / REG_KEYNAME_SIZE) return (int)0xffffffffu;
    size_t bytes = (size_t)count * REG_KEYNAME_SIZE;
    char *names = (char *)calloc(bytes, 1u);
    if (names == NULL) return (int)0xffffffffu;
    int keys_rc = sceRegGetKeys(category, names, count);
    int result = (int)0xffffffffu;
    if (keys_rc == 0) {
        for (int i = 0; i < count; i++) {
            char *name = names + (size_t)i * REG_KEYNAME_SIZE;
            if (memchr(name, '\0', REG_KEYNAME_SIZE) == NULL ||
                !registry_name_is_safe(name)) continue;
            REGHANDLE key = 0;
            unsigned int type = 0;
            SceSize size = 0;
            if (sceRegGetKeyInfo(category, name, &key, &type, &size) == 0 &&
                type != REG_TYPE_DIR && size > 1u) {
                uint8_t one_byte = 0;
                result = sceRegGetKeyValue(category, key, &one_byte, 1u);
                break;
            }
        }
    }
    free(names);
    return result;
}

/* Category names at or beyond the firmware's stored-name limit may be truncated copies;
 * measured on PSP-3000 6.6.1 (2026-10-09): stored names are cut to 26 bytes and such a
 * category could not be reopened by its cut name. */
#define REGISTRY_SAFE_NAME_MAX 26u
static int registry_category_reopenable(const char *name) {
    size_t length = strlen(name);
    if (length == 0u || length >= REGISTRY_SAFE_NAME_MAX) return 0;
    if (strncmp(name, "__NAKAGAWA", 10) == 0) return 0;
    return 1;
}

static void walk_registry_category(int emulated, REGHANDLE registry,
                                   const char *api_path, const char *display_path) {
    REGHANDLE category = 0;
    probe_step(emulated, "registry-readonly", display_path);
    int open_rc = sceRegOpenCategory(registry, api_path, 1, &category);
    if (open_rc < 0) return;
    int key_count = -1;
    int count_rc = sceRegGetKeysNum(category, &key_count);
    uint32_t category_count = key_count < 0 ? 0xffffffffu : (uint32_t)key_count;
    char record_id[48];
    snprintf(record_id, sizeof(record_id), "registry-category-%04u",
             (unsigned int)s_registry_categories);
    uint32_t out[2] = {category_count, (uint32_t)0xffffffffu};
    if (count_rc != 0 || key_count < 0 ||
        (size_t)key_count > (size_t)-1 / REG_KEYNAME_SIZE) {
        emit_registry_record(emulated, record_id, "FAIL", (uint32_t)count_rc,
                             out, 2, display_path, NULL, 0);
        s_registry_categories++;
        (void)sceRegCloseCategory(category);
        return;
    }
    size_t bytes = (size_t)key_count * REG_KEYNAME_SIZE;
    char *names = bytes ? (char *)calloc(bytes, 1u) : NULL;
    if (bytes != 0u && names == NULL) {
        emit_registry_record(emulated, record_id, "FAIL", (uint32_t)count_rc,
                             out, 2, display_path, NULL, 0);
        s_registry_categories++;
        (void)sceRegCloseCategory(category);
        return;
    }
    int keys_rc = sceRegGetKeys(category, names, key_count);
    out[1] = (uint32_t)keys_rc;
    emit_registry_record(emulated, record_id,
                         count_rc == 0 && keys_rc == 0 ? "PASS" : "FAIL",
                         (uint32_t)count_rc, out, 2, display_path, NULL, 0);
    s_registry_categories++;
    if (keys_rc == 0) {
        for (int i = 0; i < key_count; i++) {
            char *name = names + (size_t)i * REG_KEYNAME_SIZE;
            if (memchr(name, '\0', REG_KEYNAME_SIZE) == NULL ||
                !registry_name_is_safe(name)) continue;
            int is_directory = registry_read_key(emulated, category, display_path, name,
                                                  s_registry_keys);
            /* Never descend into a category that might not round-trip: the firmware stores
             * category names cut to REGISTRY_SAFE_NAME_MAX bytes, a cut name cannot be
             * reopened, and opening a name that is not found CREATES a category (a flash
             * write). Our own stray test categories are skipped by prefix for the same reason. */
            if (is_directory && registry_category_reopenable(name)) {
                size_t api_length = strlen(api_path);
                size_t name_length = strlen(name);
                size_t display_length = strlen(display_path);
                if (api_length > (size_t)-1 - name_length - 2u ||
                    display_length > (size_t)-1 - name_length - 2u) continue;
                char *next_api = (char *)malloc(api_length + name_length + 2u);
                char *next_display = (char *)malloc(display_length + name_length + 2u);
                if (next_api != NULL && next_display != NULL) {
                    snprintf(next_api, api_length + name_length + 2u,
                             "%s/%s", api_path, name);
                    snprintf(next_display, display_length + name_length + 2u,
                             "%s/%s", display_path, name);
                    walk_registry_category(emulated, registry, next_api, next_display);
                }
                free(next_api);
                free(next_display);
            }
        }
    }
    free(names);
    (void)sceRegCloseCategory(category);
}

/* Registry handle-exhaustion bound.  The previous revision opened handles
   until sceRegOpenRegistry failed, with no bound; on a PSP-3000 the launch
   went silent after registry-open.  256 open handles is far beyond what the
   probe or a game needs at once; reaching the cap without a failure is
   recorded as the measured outcome "no exhaustion within the cap". */
#define REGISTRY_OPEN_CAP 256u
static REGHANDLE s_registry_handles[REGISTRY_OPEN_CAP];

#define REGISTRY_STEP(step) probe_step(emulated, "registry-readonly", (step))

static void run_registry_readonly(int emulated) {
    struct RegParam param;
    memset(&param, 0, sizeof(param));
    param.regtype = 1u;
    memcpy(param.name, SYSTEM_REGISTRY, sizeof(SYSTEM_REGISTRY));
    param.namelen = sizeof(SYSTEM_REGISTRY) - 1u;
    param.unk2 = 1u;
    param.unk3 = 1u;
    REGHANDLE registry = 0;
    REGISTRY_STEP("open-registry");
    const int open_rc = sceRegOpenRegistry(&param, 1, &registry);
    uint32_t open_out[2] = {1u, param.regtype};
    emit_registry_record(emulated, "registry-open", open_rc == 0 ? "PASS" : "FAIL",
                         (uint32_t)open_rc, open_out, 2, NULL, NULL, 0);

    int unknown_category_rc = (int)0xffffffffu;
    int unknown_key_rc = (int)0xffffffffu;
    int small_buffer_rc = (int)0xffffffffu;
    int open_failure_rc = 0;
    uint32_t opened_handles = 0;
    uint32_t exhausted = 0;
    REGHANDLE config = 0;
    int config_rc = (int)0xffffffffu;
    if (open_rc == 0) {
        /* No probe may open a category that enumeration did not report: on PSP-3000 6.6.1
         * sceRegOpenCategory on a missing category CREATES it (and persists it to flash),
         * even with mode 1, so an "unknown category" probe is a registry write. The
         * unknown-category error code is therefore not measured here (left 0xffffffff). */
        REGISTRY_STEP("open-config");
        config_rc = sceRegOpenCategory(registry, "/CONFIG", 1, &config);
        if (config_rc == 0) {
            REGHANDLE key = 0;
            unsigned int type = 0;
            SceSize size = 0;
            REGISTRY_STEP("unknown-key");
            unknown_key_rc = sceRegGetKeyInfo(config,
                "__NAKAGAWA_ORACLE_UNKNOWN_KEY__", &key, &type, &size);
        }

        REGISTRY_STEP("handle-exhaustion");
        while (opened_handles < REGISTRY_OPEN_CAP) {
            REGHANDLE handle = 0;
            int rc = sceRegOpenRegistry(&param, 1, &handle);
            if (rc < 0) {
                open_failure_rc = rc;
                exhausted = 1u;
                break;
            }
            s_registry_handles[opened_handles++] = handle;
        }
        REGISTRY_STEP("handle-release");
        for (uint32_t i = 0; i < opened_handles; i++) {
            (void)sceRegCloseRegistry(s_registry_handles[i]);
        }
        if (config_rc == 0) {
            REGISTRY_STEP("small-buffer");
            small_buffer_rc = registry_probe_small_buffer(config);
        }
    }
    /* out4 counts the opens that succeeded; out6 is 1 when an open failed
       before the cap (out3 is then its result) and 0 when all
       REGISTRY_OPEN_CAP opens succeeded (out3 is then 0). */
    uint32_t error_out[7] = {
        (uint32_t)unknown_category_rc,
        (uint32_t)unknown_key_rc,
        (uint32_t)small_buffer_rc,
        (uint32_t)open_failure_rc,
        opened_handles,
        REGISTRY_OPEN_CAP,
        exhausted,
    };
    emit_registry_record(emulated, "registry-errors", "PASS", (uint32_t)open_rc,
                         error_out, 7, NULL, NULL, 0);
    if (config_rc == 0) {
        REGISTRY_STEP("walk-config");
        walk_registry_category(emulated, registry, "/CONFIG", "CONFIG");
    }
    if (config_rc == 0) (void)sceRegCloseCategory(config);

    /* The bogus-handle call runs last, after every other measurement has
       been recorded, under its own step marker: if the kernel faults or never
       returns on a forged handle, nothing else is lost. */
    int bad_handle_rc = (int)0xffffffffu;
    int bad_handle_count = -1;
    if (open_rc == 0) {
        REGISTRY_STEP("bad-handle");
        bad_handle_rc = sceRegGetKeysNum((REGHANDLE)0xffffffffu, &bad_handle_count);
    }
    uint32_t bad_out[2] = {(uint32_t)bad_handle_rc, (uint32_t)bad_handle_count};
    emit_registry_record(emulated, "registry-bad-handle",
                         open_rc == 0 ? "PASS" : "SKIP", (uint32_t)bad_handle_rc,
                         bad_out, 2, NULL, NULL, 0);
    if (open_rc == 0) (void)sceRegCloseRegistry(registry);
    uint32_t done_out[3] = {
        s_registry_categories,
        s_registry_keys,
        s_registry_records,
    };
    emit_registry_record(emulated, "registry-done", "PASS", 0,
                         done_out, 3, NULL, NULL, 0);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_BREAK_CONTINUE
static void emit_ge_control(int emulated, const char *case_id, const char *status,
                            uint32_t result, const uint32_t *out, size_t count) {
    emit_record_extended(emulated, "PSP-GE-CONTROL-001", case_id, status,
                         result, out, count);
}

/* Record whether the GE is idle and, when it is not, reset every queue and
   record whether that reset reached idle within the bound.  Emits four
   outputs: reset issued, sceGeBreak(1) result, last draw state, elapsed us. */
#define GE_QUIESCE_OUTS 4
static void emit_ge_quiesce(int emulated, const char *case_id) {
    probe_step(emulated, "ge-break-continue", case_id);
    GeIdleWait w = ge_wait_idle(-1, 0u);
    uint32_t reset_issued = 0u;
    int reset_rc = 0;
    if (!w.idle) {
        reset_issued = 1u;
        reset_rc = ge_reset_queues(&w);
    }
    uint32_t out[GE_QUIESCE_OUTS] = {
        reset_issued, (uint32_t)reset_rc, (uint32_t)w.draw_state, w.elapsed_us,
    };
    emit_ge_control(emulated, case_id, w.idle ? "PASS" : "TIMEOUT",
                    (uint32_t)reset_rc, out, GE_QUIESCE_OUTS);
}

static void run_ge_break_continue(int emulated) {
    PspGeBreakParam param;
    memset(&param, 0xa5, sizeof(param));
    int rc = sceGeBreak(0, &param);
    uint32_t out2[3] = {(uint32_t)rc, param.buf[0], (uint32_t)sizeof(param)};
    emit_ge_control(emulated, "ge-break-no-active-list", "PASS",
                    (uint32_t)rc, out2, 2);

    rc = sceGeContinue();
    uint32_t out1[1] = {(uint32_t)rc};
    emit_ge_control(emulated, "ge-continue-no-paused-list", "PASS",
                    (uint32_t)rc, out1, 1);

    memset(&param, 0xa5, sizeof(param));
    rc = sceGeBreak(-1, &param);
    out2[0] = (uint32_t)rc;
    out2[1] = param.buf[0];
    emit_ge_control(emulated, "ge-break-invalid-mode", "PASS",
                    (uint32_t)rc, out2, 2);

    const uint32_t vram = (uint32_t)(uintptr_t)sceGeEdramGetAddr();
    const uint32_t src = (uint32_t)(uintptr_t)s_ge_src;
    const uint32_t dst = vram + 0x00100000u;
    for (uint32_t i = 0; i < GE_TILE_W * GE_TILE_H; i++) {
        s_ge_src[i] = 0x5a5a5a5au;
    }
    /* Build the list first, then write the data cache back: the GE fetches the
       list and the transfer source from RAM, not from the CPU cache.  The
       previous revision wrote the list after the writeback, so its words could
       still sit in dirty cache lines while the GE read the zero-initialised
       buffer (NOP words) and ran on past the list without meeting FINISH/END;
       such a list never completes. */
    (void)ge_build_list(src, dst);
    sceKernelDcacheWritebackAll();
    const uint32_t list = (uint32_t)(uintptr_t)s_ge_list;
    probe_step(emulated, "ge-break-continue", "break-active-list");
    int qid = sceGeListEnQueue((const void *)(uintptr_t)list, NULL, -1, NULL);
    int break_rc = qid >= 0 ? sceGeBreak(0, &param) : qid;
    int list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
    int draw_state = qid >= 0 ? sceGeDrawSync(1) : qid;
    uint32_t list_out[3] = {(uint32_t)qid, (uint32_t)break_rc,
                            (uint32_t)list_state};
    uint32_t draw_out[3] = {(uint32_t)qid, (uint32_t)break_rc,
                            (uint32_t)draw_state};
    const char *paused_status = qid >= 0 ? "PASS" : "SKIP";
    emit_ge_control(emulated, "ge-list-sync-paused", paused_status,
                    (uint32_t)list_state, list_out, 3);
    emit_ge_control(emulated, "ge-draw-sync-paused", paused_status,
                    (uint32_t)draw_state, draw_out, 3);
    /* Continue the broken list and wait for it, bounded.  The list ends in
       FINISH then END (the terminator sceGuFinish emits) and was enqueued
       with no stall address, so once the GE runs it again nothing in the list
       can hold it; there is no stall to release, so the stall address is not
       touched.  The previous revision ignored the continue result, moved a
       stall the list never had, and blocked in sceGeListSync(qid, 0); on a
       PSP-3000 that wait never returned after the break left the list in
       state 4 with sceGeDrawSync(1) at 2.  A blocking list sync returns only
       once the list reaches PSP_GE_LIST_DONE, which a list the GE read from
       stale memory (see the writeback above) never reaches.  The continue
       result, the drain states and the elapsed time recorded here show
       whether the corrected list now completes after a break and continue. */
    probe_step(emulated, "ge-break-continue", "continue-drain");
    const int continue_rc = qid >= 0 ? sceGeContinue() : qid;
    const GeIdleWait drain = ge_wait_idle(qid, qid >= 0 ? GE_IDLE_DEADLINE_US : 0u);
    uint32_t drain_out[5] = {
        (uint32_t)qid, (uint32_t)continue_rc, (uint32_t)drain.list_state,
        (uint32_t)drain.draw_state, drain.elapsed_us,
    };
    emit_ge_control(emulated, "ge-continue-drain",
                    qid < 0 ? "SKIP" : drain.idle ? "PASS" : "TIMEOUT",
                    (uint32_t)continue_rc, drain_out, 5);
    emit_ge_quiesce(emulated, "ge-quiesce-after-continue");

    probe_step(emulated, "ge-break-continue", "cancel-stalled-list");
    qid = sceGeListEnQueue((const void *)(uintptr_t)list,
                           (void *)(uintptr_t)list, -1, NULL);
    const int cancel_rc = qid >= 0 ? sceGeListDeQueue(qid) : qid;
    list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
    draw_state = qid >= 0 ? sceGeDrawSync(1) : qid;
    list_out[0] = (uint32_t)qid;
    list_out[1] = (uint32_t)cancel_rc;
    list_out[2] = (uint32_t)list_state;
    draw_out[0] = (uint32_t)qid;
    draw_out[1] = (uint32_t)cancel_rc;
    draw_out[2] = (uint32_t)draw_state;
    const char *cancel_status = qid >= 0 && cancel_rc >= 0 ? "PASS" : "SKIP";
    emit_ge_control(emulated, "ge-list-sync-cancelled", cancel_status,
                    (uint32_t)list_state, list_out, 3);
    emit_ge_control(emulated, "ge-draw-sync-cancelled", cancel_status,
                    (uint32_t)draw_state, draw_out, 3);
    emit_ge_quiesce(emulated, "ge-quiesce-after-cancel");

    uint32_t done = 10;
    emit_record_extended(emulated, "PSP-GE-CONTROL-001", "ge-break-continue-done",
                         "PASS", 0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_KERNEL_MISC
static void run_kernel_misc(int emulated) {
    const SceInt64 clock = (SceInt64)1234567890;
    unsigned int usec_low = 0;
    unsigned int usec_high = 0;
    int rc = sceKernelSysClock2USecWide(clock, &usec_low, &usec_high);
    uint32_t clock_out[4] = {
        (uint32_t)clock, (uint32_t)((uint64_t)clock >> 32),
        (uint32_t)usec_low, (uint32_t)usec_high,
    };
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "sysclock-wide",
                         "PASS", (uint32_t)rc, clock_out, 4);

    int mode = -1;
    rc = sceCtrlGetSamplingMode(&mode);
    uint32_t ctrl_out[2] = {(uint32_t)mode, 0u};
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "ctrl-sampling-mode",
                         "PASS", (uint32_t)rc, ctrl_out, 2);

    PspDebugProfilerRegs *profiler = sceKernelReferThreadProfiler();
    uint32_t profiler_out[2] = {
        (uint32_t)(uintptr_t)profiler, profiler != NULL ? 1u : 0u,
    };
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "thread-profiler",
                         "PASS", (uint32_t)(uintptr_t)profiler, profiler_out, 2);
    profiler = sceKernelReferGlobalProfiler();
    profiler_out[0] = (uint32_t)(uintptr_t)profiler;
    profiler_out[1] = profiler != NULL ? 1u : 0u;
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "global-profiler",
                         "PASS", (uint32_t)(uintptr_t)profiler, profiler_out, 2);

    probe_step(emulated, "kernel-misc", "vtimer-basic");
    SceUID timer = sceKernelCreateVTimer("oracle-vtimer", NULL);
    SceKernelSysClock before = {0};
    SceKernelSysClock after = {0};
    SceKernelSysClock stopped = {0};
    int before_rc = sceKernelGetVTimerTime(timer, &before);
    int start_rc = sceKernelStartVTimer(timer);
    sceKernelDelayThread(10000u);
    int after_rc = sceKernelGetVTimerTime(timer, &after);
    int stop_rc = sceKernelStopVTimer(timer);
    int stopped_rc = sceKernelGetVTimerTime(timer, &stopped);
    int delete_rc = sceKernelDeleteVTimer(timer);
    uint32_t timer_out[13] = {
        (uint32_t)timer, (uint32_t)before_rc, before.low, before.hi,
        (uint32_t)start_rc, (uint32_t)after_rc, after.low, after.hi,
        (uint32_t)stop_rc, (uint32_t)stopped_rc, stopped.low, stopped.hi,
        (uint32_t)delete_rc,
    };
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "vtimer-basic",
                         "PASS", (uint32_t)timer, timer_out, 13);

    probe_step(emulated, "kernel-misc", "display-basic");
    int hold_rc = sceDisplaySetHoldMode(0);
    int wait_rc = sceDisplayWaitVblankStartMultiCB(1u);
    uint32_t display_out[4] = {0u, (uint32_t)hold_rc, 1u, (uint32_t)wait_rc};
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "display-basic",
                         "PASS", (uint32_t)wait_rc, display_out, 4);

    probe_step(emulated, "kernel-misc", "impose-basic");
    int charging = (int)0x5a5a5a5a;
    int icon_status = (int)0x5a5a5a5a;
    int battery_rc = sceImposeBatteryIconStatus(&charging, &icon_status);
    int popup_before = sceImposeGetUMDPopup();
    int popup_set_rc = popup_before >= 0 ?
        sceImposeSetUMDPopup(popup_before) : popup_before;
    uint32_t impose_out[5] = {
        (uint32_t)charging, (uint32_t)icon_status, (uint32_t)popup_before,
        (uint32_t)popup_set_rc, popup_before >= 0 ? 1u : 0u,
    };
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "impose-basic",
                         "PASS", (uint32_t)battery_rc, impose_out, 5);

    uint32_t done = 7;
    emit_record_extended(emulated, "PSP-KERNEL-MISC-001", "kernel-misc-done",
                         "PASS", 0, &done, 1);
}
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    s_probe_main_thread = sceKernelGetThreadId();
#if defined(__mips__)
    uint32_t boot_fcr31 = 0;
    /* Save FCR31 before probe code; the FPU vector case then starts from zero. */
    __asm__ volatile("cfc1 %0, $31" : "=r"(boot_fcr31) :: "memory");
    s_boot_fcr31 = boot_fcr31;
    s_have_fcr31_state = 1;
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
#endif
#endif
    s_boot_cpu_mhz = scePowerGetCpuClockFrequencyInt();
    s_boot_bus_mhz = scePowerGetBusClockFrequencyInt();
    s_have_clock_state = s_boot_cpu_mhz > 0 && s_boot_bus_mhz > 0;
    const int emulated = emulator_present();
    /* Unbuffered stdout so a probe-induced exception stays attributable to
       the exact record instead of losing buffered output. Zero semantic
       effect on emitted records. */
    setvbuf(stdout, NULL, _IONBF, 0);
    char line[320];

    /* uint32_t is `unsigned long` in the PSP newlib ABI, so %x must be fed an
       explicitly-converted unsigned int or psp-gcc warns under -Wformat.
       The device reports its build commit so the host can compare it with the
       staged source. It cannot hash its module image, so binary_sha256 remains
       an explicit placeholder and is recorded as a host-measured digest. */
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=" PROBE_BUILD_COMMIT " fixture=%s\n",
             emulated ? "ppsspp" : "psp", FIXTURE_BUILD_ID);
    emit(emulated, line);
#ifdef PROBE_HOST0_LOG
    if (!emulated) {
        SceUID fd = sceIoOpen(PROBE_HOST0_LOG,
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, line, strlen(line));
            sceIoClose(fd);
        }
    }
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CALLBACK
    uint32_t out0 = 0;
    uint32_t out1 = 0;
    uint32_t out2 = 0;
    uint32_t out3 = 0;
    const int pass = run_callback_case(&out0, &out1, &out2, &out3);
    emit_test(emulated, "callback-notify-check", pass, pass ? 1u : 0u,
              out0, out1, out2, out3);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_CANCEL
    uint32_t out0 = 0;
    uint32_t out1 = 0;
    uint32_t out2 = 0;
    uint32_t out3 = 0;
    const int pass = (int)run_wait_cancel_case(&out0, &out1, &out2, &out3);
    emit_test(emulated, "wait-cancel", pass, pass ? 1u : 0u,
              out0, out1, out2, out3);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_LIFECYCLE
    uint32_t out0 = 0;
    uint32_t out1 = 0;
    uint32_t out2 = 0;
    uint32_t out3 = 0;
    const int pass = run_thread_lifecycle_case(&out0, &out1, &out2, &out3);
    emit_test(emulated, "thread-lifecycle", pass, pass ? 1u : 0u,
              out0, out1, out2, out3);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE
    uint32_t out[10] = {0};
    const int pass = (int)run_thread_delete_case(&out[0], &out[1], &out[2], &out[3],
                                                 &out[4], &out[5], &out[6], &out[7], &out[8], &out[9]);
    emit_test_extended(emulated, "thread-delete-lifecycle", pass, pass ? 1u : 0u,
                       out, sizeof(out) / sizeof(out[0]));
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_FOLLOWUP
    uint32_t out[17] = {0};
    const int pass = (int)run_thread_delete_followup_case(
        &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6], &out[7],
        &out[8], &out[9], &out[10], &out[11], &out[12], &out[13], &out[14],
        &out[15], &out[16], 0);
    emit_test_extended(emulated, "thread-delete-followup", pass, pass ? 1u : 0u,
                       out, 15);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_EXPLICIT
    uint32_t out[17] = {0};
    const int pass = (int)run_thread_delete_followup_case(
        &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6], &out[7],
        &out[8], &out[9], &out[10], &out[11], &out[12], &out[13], &out[14],
        &out[15], &out[16], 1);
    emit_test_extended(emulated, "thread-delete-explicit", pass, pass ? 1u : 0u,
                       out, 15);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY
    uint32_t out[17] = {0};
    const int pass = (int)run_thread_delete_followup_case(
        &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6], &out[7],
        &out[8], &out[9], &out[10], &out[11], &out[12], &out[13], &out[14],
        &out[15], &out[16], 2);
    emit_test_extended(emulated, "thread-delete-boundary", pass, pass ? 1u : 0u,
                       out, sizeof(out) / sizeof(out[0]));
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
    run_dmac_concurrency(emulated);
#elif PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
      PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
    run_dmac_invalid_tail(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_S0
    run_dmac_invalid_tail(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT
    run_display_mask_vcount(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY
    run_display_mask_duty(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK
    run_display_ge_mask(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_TRANSPORT_WRITE
    uint32_t tout[5] = {0};
    const int tpass = run_transport_write_case(emulated, tout);
    if (tpass != 0) {
        emit_record_extended(emulated, "PSP-TRANSPORT-001",
                             "host0-write-readback", "FAIL",
                             (uint32_t)tpass, tout, 5);
    }
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_EXIT_DELETE
    run_thread_exit_delete(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
    run_dmac_survey(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX || \
      PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL
    run_dmac_size_matrix(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CTRL_CLOCK
    run_ctrl_clock(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
    run_fpu_vector(emulated, boot_fcr31);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE
    run_vfpu_compare(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_TEARDOWN_TEST
    run_teardown_test(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
    run_io_matrix(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
    run_audio_query(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
    run_cache_alias(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MODEL_PROFILE
    run_model_profile(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_LATE
    run_display_wait_late(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_WAIT_PRIORITY
    run_display_wait_priority(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_VBLANK_WINDOW
    run_display_vblank_window(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_REFER_UNLOCKED
    uint32_t out0 = 0, out1 = 0, out2 = 0, out3 = 0, out4 = 0;
    const int pass = (int)run_mutex_refer_unlocked_case(&out0, &out1, &out2, &out3, &out4);
    uint32_t out[5] = {out0, out1, out2, out3, out4};
    emit_mutex_test(emulated, "mutex-refer-unlocked", pass, pass ? 1u : 0u, out, 5);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_TIMEOUT_QUANTA
    uint32_t out[17] = {0};
    const int pass = (int)run_mutex_timeout_quanta_case(out, 17);
    emit_mutex_test(emulated, "mutex-timeout-quanta", pass, pass ? 1u : 0u, out, 17);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_PRIORITY_INHERITANCE
    uint32_t out[7] = {0};
    const int pass = (int)run_mutex_priority_inheritance_case(&out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6]);
    emit_mutex_test(emulated, "mutex-priority-inheritance", pass, pass ? 1u : 0u, out, 7);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MUTEX_INTERRUPT_CONTEXT
    run_mutex_interrupt_context_case(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_MBX_DELETE_WAIT
    run_mbx_delete_wait(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN
    run_ge_nan(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CELLS
    run_dmac_cells(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO
    run_delay_zero(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_KERNEL_ALARM
    run_kernel_alarm(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_SCHEDULER
    run_thread_scheduler(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_OUTCOMES
    run_wait_outcomes(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_BREAK_CONTINUE
    run_ge_break_continue(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_REFER_STATUS_SIZE
    run_refer_status_size(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_READONLY
    run_registry_readonly(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_KERNEL_MISC
    run_kernel_misc(emulated);
#else
    const uint32_t sum = nakagawa_psp_oracle_sum_u32(100);
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SMOKE-001 case_id=sum-1-to-100 "
             "status=%s result=0x%08x out0=0x%08x\n",
             sum == 5050 ? "PASS" : "FAIL", (unsigned int)sum, sum == 5050 ? 1u : 0u);
    probe_emit_durable(emulated, line, strlen(line));
#endif

    probe_teardown(emulated);
}
#endif /* defined(__mips__) */
