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
#if PSP_ORACLE_CASE == 66 || PSP_ORACLE_CASE == 72
#include <pspreg.h>
#endif
#if PSP_ORACLE_CASE == 74 || PSP_ORACLE_CASE == 77
#include <pspumd.h>
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
#define PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS 71
#define PSP_ORACLE_CASE_REGISTRY_FONT 72
#define PSP_ORACLE_CASE_FONT_FILES 73
#define PSP_ORACLE_CASE_UMD_IO 74
#define PSP_ORACLE_CASE_UMD_RAW_READ 77
#define PSP_ORACLE_CASE_FONT_LIST 75
#define PSP_ORACLE_CASE_VRAM_MIRROR 76
/* #1106: a late-loaded PRX that owns a semaphore-blocked waiter, an event flag and a callback. Its
   module_stop returns 0 without deleting, waking or otherwise touching them. PSPLink's modstop and
   modunld then measure what survives. Never returns from main, so the probe teardown never runs. */
#define PSP_ORACLE_CASE_LATE_PRX_TARGET 110
/* #1096: one bounded AT3 decode of a host0-staged stream. The bytes come from host0, never from the source. */
#define PSP_ORACLE_CASE_AT3_DECODE 111
/* Runtime handlers that answered success unconditionally: sceKernelVolatileMemUnlock and
   sceUtilityOskUpdate. Non-blocking and dialog-free; see run_fake_success. */
#define PSP_ORACLE_CASE_FAKE_SUCCESS 112
/* #1055: project and console PGFs opened in the resident font library; see run_font_userpgf. */
#define PSP_ORACLE_CASE_FONT_USERPGF 113
/* #1296: GE execution time by command mix and CPU-visible completion; see run_ge_timing. */
#define PSP_ORACLE_CASE_GE_TIMING 114
/* #1296: CPU instruction-class and per-frame syscall timing; synthetic buffers only. */
#define PSP_ORACLE_CASE_CPU_TIMING 115

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
#define PSP_ORACLE_CASE_LLSC_LINK 69
#define PSP_ORACLE_CASE_FPU_CC 70

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
      PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE || \
      PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS || \
      PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING
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
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_RAW_READ
    /* The raw UMD measurement is PSP-only; do not issue emulator devctls. */
    return 0;
#else
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", EMULATOR_DEVCTL_IS_EMULATOR, NULL, 0, &flag, sizeof(flag)) < 0) {
        return 0;
    }
    return flag == 1;
#endif
}

static void emit(int emulated, const char *text) {
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_RAW_READ
    /* This case writes durable records to host0 and has no emulator output route. */
    (void)emulated;
    (void)text;
#else
    if (emulated) {
        sceIoDevctl("emulator:", EMULATOR_DEVCTL_SEND_OUTPUT, (void *)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
        fflush(stdout);
    }
#endif
}

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
#define PROBE_HOST0_LOG "host0:/fpu_vector_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
#define PROBE_HOST0_LOG "host0:/io_matrix_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
#define PROBE_HOST0_LOG "host0:/audio_query_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
#define PROBE_HOST0_LOG "host0:/cache_alias_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
/* The three concurrency records are durable too, so a PSPLink session needs no stdout capture. */
#define PROBE_HOST0_LOG "host0:/dmac_concurrency_log.txt"
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
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS
#define PROBE_HOST0_LOG "host0:/vfpu_transcendental_corpus_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_FONT
#define PROBE_HOST0_LOG "host0:/registry_font_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_FILES
#define PROBE_HOST0_LOG "host0:/font_files_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_IO
#define PROBE_HOST0_LOG "host0:/umd_io_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_RAW_READ
#define PROBE_HOST0_LOG "host0:/umd_raw_read_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_LIST
#define PROBE_HOST0_LOG "host0:/font_list_log.txt"
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
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_LLSC_LINK
#define PROBE_HOST0_LOG "host0:/llsc_link_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_CC
#define PROBE_HOST0_LOG "host0:/fpu_cc_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR
#define PROBE_HOST0_LOG "host0:/vram_mirror_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AT3_DECODE
#define PROBE_HOST0_LOG "host0:/at3_decode_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FAKE_SUCCESS
#define PROBE_HOST0_LOG "host0:/fake_success_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_USERPGF
#define PROBE_HOST0_LOG "host0:/font_userpgf_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_TIMING
#define PROBE_HOST0_LOG "host0:/ge_timing_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING
#define PROBE_HOST0_LOG "host0:/cpu_timing_log.txt"
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
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR
    char line[3072];
#else
    char line[1024];
#endif
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
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR
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
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR
#define DEFERRED_MAX_OUT 128
#define VRAM_MIRROR_SIZE 0x00200000u
/* #1049 caveat: extra depth-buffer offset past the display-derived layout. The default 0 keeps the
   measured zbp 0x0cc000 (bits 6 and 13 clear). A bias with bit 6 or bit 13 set separates the
   XOR-on-absolute-offset hypothesis from the XOR-within-the-depth-buffer hypothesis. */
#ifndef VRAM_MIRROR_DEPTH_BIAS
#define VRAM_MIRROR_DEPTH_BIAS 0x00000000u
#endif
#define VRAM_MIRROR_ROW_BYTES 0x400u
#define VRAM_MIRROR_ROW_WORDS (VRAM_MIRROR_ROW_BYTES / 4u)
#define VRAM_MIRROR_CPU_DENSE_OFFSET 0x00088000u
#define VRAM_MIRROR_CPU_DENSE_ROWS 8u
#define VRAM_MIRROR_CPU_PAIR_REGION_COUNT 4u
#define VRAM_MIRROR_CPU_PAIR_ROWS 2u
#define VRAM_MIRROR_CPU_SWEEP_FIRST_BIT 2u
#define VRAM_MIRROR_CPU_SWEEP_LAST_BIT 20u
#define VRAM_MIRROR_CPU_SWEEP_BIT_COUNT \
    (VRAM_MIRROR_CPU_SWEEP_LAST_BIT - VRAM_MIRROR_CPU_SWEEP_FIRST_BIT + 1u)
#define VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS 8u
#define VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT \
    (VRAM_MIRROR_CPU_DENSE_ROWS * VRAM_MIRROR_ROW_WORDS + \
     VRAM_MIRROR_CPU_PAIR_REGION_COUNT * VRAM_MIRROR_CPU_PAIR_ROWS * \
         VRAM_MIRROR_ROW_WORDS + \
     2u * VRAM_MIRROR_CPU_SWEEP_BIT_COUNT)
#define VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT \
    ((VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT + VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS - 1u) / \
     VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS)
#define VRAM_MIRROR_WRITE_ALIAS_COUNT 6u
#define VRAM_MIRROR_WRITE_SAMPLE_COUNT 3u
#define VRAM_MIRROR_WRITE_RECORD_COUNT \
    (VRAM_MIRROR_WRITE_ALIAS_COUNT * VRAM_MIRROR_WRITE_SAMPLE_COUNT)
#define VRAM_MIRROR_FRAME_BYTES (512u * 272u * 2u)
#define VRAM_MIRROR_DEPTH_STRIDE 512u
#define VRAM_MIRROR_DEPTH_ROW_COUNT 10u
#define VRAM_MIRROR_DEPTH_ALIAS_COUNT 2u
#define VRAM_MIRROR_DEPTH_CHUNK_PIXELS 32u
#define VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT \
    (VRAM_MIRROR_DEPTH_STRIDE / VRAM_MIRROR_DEPTH_CHUNK_PIXELS)
#define VRAM_MIRROR_DEPTH_RECORD_COUNT \
    (VRAM_MIRROR_DEPTH_ROW_COUNT * VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT)
#define VRAM_MIRROR_COLOR_ROW_COUNT 3u
#define VRAM_MIRROR_COLOR_CHUNK_PIXELS 64u
#define VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT \
    (VRAM_MIRROR_DEPTH_STRIDE / VRAM_MIRROR_COLOR_CHUNK_PIXELS)
#define VRAM_MIRROR_COLOR_RECORD_COUNT \
    (VRAM_MIRROR_COLOR_ROW_COUNT * VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT)
#define VRAM_MIRROR_GE_SYNC_COUNT 4u
#define VRAM_MIRROR_INFO_OUT_COUNT 53u
#define VRAM_MIRROR_SYNC_OUT_COUNT 27u
#define VRAM_MIRROR_CLEAR_DEPTH 0x5a5au
#define VRAM_MIRROR_PATTERN_PREFIX 0xa5000000u
#define VRAM_MIRROR_MARKER_PREFIX 0xb6000000u
#define VRAM_MIRROR_WRITE_OFFSET_0 0x000100u
#define VRAM_MIRROR_WRITE_OFFSET_1 0x044100u
#define VRAM_MIRROR_WRITE_OFFSET_2 0x088100u
#define VRAM_MIRROR_CACHED_ALIAS_COUNT 4u
#define VRAM_MIRROR_DEPTH_ROW_Y0 0u
#define VRAM_MIRROR_DEPTH_ROW_Y1 7u
#define VRAM_MIRROR_DEPTH_ROW_Y2 8u
#define VRAM_MIRROR_DEPTH_ROW_Y3 9u
#define VRAM_MIRROR_DEPTH_ROW_Y4 15u
#define VRAM_MIRROR_DEPTH_ROW_Y5 16u
#define VRAM_MIRROR_DEPTH_ROW_Y6 100u
#define VRAM_MIRROR_DEPTH_ROW_Y7 101u
#define VRAM_MIRROR_DEPTH_ROW_Y8 108u
#define VRAM_MIRROR_DEPTH_ROW_Y9 109u
#define VRAM_MIRROR_COLOR_ROW_Y0 0u
#define VRAM_MIRROR_COLOR_ROW_Y1 8u
#define VRAM_MIRROR_COLOR_ROW_Y2 100u
#define VRAM_MIRROR_DEPTH_RANGE_NEAR 65535u
#define VRAM_MIRROR_DEPTH_RANGE_FAR 0u
#define VRAM_MIRROR_DEPTH_TEST_ENABLED 1u
#define VRAM_MIRROR_SEMANTIC_RECORDS \
    (1u + VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT + VRAM_MIRROR_WRITE_RECORD_COUNT + \
     VRAM_MIRROR_GE_SYNC_COUNT + VRAM_MIRROR_DEPTH_RECORD_COUNT + \
     VRAM_MIRROR_COLOR_RECORD_COUNT + 1u)
#define DEFERRED_MAX_RECORDS (VRAM_MIRROR_SEMANTIC_RECORDS + 1u)
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN
#define DEFERRED_MAX_RECORDS 24
#define DEFERRED_MAX_OUT 6
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
#define DEFERRED_MAX_RECORDS 16
#define DEFERRED_MAX_OUT 8
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DELAY_ZERO
#define DEFERRED_MAX_RECORDS 4
#define DEFERRED_MAX_OUT 10
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS
#define DEFERRED_MAX_RECORDS 128
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
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_TIMING || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING
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

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_TIMING || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING
/* GE teardown for every case that calls sceGuInit, on every path out of it.
 *
 * sceGuInit (PSPSDK src/gu/sceGuInit.c; pspsdk e62057d9, the PSPDEV v20261001
 * the probe links) creates the "SceGuSignal" event flag and registers
 * libpspgu's callbackSig and callbackFin with sceGeSetCallback.  Those handlers
 * are code in this PRX.  Only sceGuTerm (src/gu/sceGuTerm.c) removes them, by
 * sceKernelDeleteEventFlag and sceGeUnsetCallback.  A case that returns
 * without it leaves the GE driver holding handlers in memory the runner then
 * unloads, and the next GE interrupt runs freed code in interrupt context.
 *
 * The order is fixed: a bounded wait for the GE to go idle; if it does not,
 * reset every queue (sceGeBreak(1)) and wait, bounded, again; then unregister
 * the case's own GE callback, if it set one; then sceGuTerm when sceGuInit
 * succeeded.  sceGuTerm runs even when the GE never reached idle, so no
 * handler outlives the module; whether it did is returned for the record.
 * sceGuTerm is skipped after a failed sceGuInit: that call already undid what
 * it had registered, and its callback ID is not valid. */
typedef struct {
    uint32_t reset_issued;  /* the GE was busy after the wait; sceGeBreak(1) was issued */
    int reset_rc;           /* sceGeBreak result; 0 when not issued                     */
    int draw_state;         /* last sceGeDrawSync(1)                                    */
    uint32_t elapsed_us;    /* time spent in both bounded waits                         */
    int idle;               /* draw_state == PSP_GE_LIST_DONE                           */
    uint32_t term_called;   /* sceGuTerm ran                                            */
} GeGuTeardown;

#define GE_GU_TEARDOWN_OUTS 5

static GeGuTeardown ge_gu_teardown(int init_rc, int own_cbid) {
    GeGuTeardown t;
    memset(&t, 0, sizeof(t));
    GeIdleWait w = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);
    t.elapsed_us = w.elapsed_us;
    if (!w.idle) {
        t.reset_issued = 1u;
        t.reset_rc = ge_reset_queues(&w);
        t.elapsed_us += w.elapsed_us;
    }
    t.draw_state = w.draw_state;
    t.idle = w.idle;
    if (own_cbid >= 0) {
        (void)sceGeUnsetCallback(own_cbid);
    }
    if (init_rc >= 0) {
        sceGuTerm();
        t.term_called = 1u;
    }
    return t;
}

/* out0 reset issued, out1 sceGeBreak result, out2 last draw state,
   out3 elapsed us, out4 sceGuTerm ran. */
static inline void ge_gu_teardown_outs(const GeGuTeardown *t,
                                       uint32_t out[GE_GU_TEARDOWN_OUTS]) {
    out[0] = t->reset_issued;
    out[1] = (uint32_t)t->reset_rc;
    out[2] = (uint32_t)t->draw_state;
    out[3] = t->elapsed_us;
    out[4] = t->term_called;
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
   follow src/rt/vfpu_interp.c (VFPU3 sub-ops 5, 6 and 7): vscmp.s yields -1.0,
   0.0 or +1.0, vsge.s and vslt.s yield 1.0 or 0.0, and a NaN operand makes
   vsge.s and vslt.s yield 0.0. A PSP-3000 measured all 45 cells on 2026-10-15
   from build e45e667b (PSP-VFPU-CMP-001, docs/HARDWARE_ORACLE.md): that build's
   model wrote 0.0 for the five vscmp NaN cells, the console orders a NaN by sign
   and magnitude instead, and the model below now carries the measured words.
   Every record carries result = the
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
    {"nan-left-quiet", 0x7fc00000u, 0x3fc00000u, {VFPU_COMPARE_ONE, 0u, 0u}},
    {"nan-right-quiet", 0x3fc00000u, 0x7fc00000u, {VFPU_COMPARE_MINUS_ONE, 0u, 0u}},
    {"nan-left-signal", 0x7f800001u, 0x3fc00000u, {VFPU_COMPARE_ONE, 0u, 0u}},
    {"nan-right-signal", 0x3fc00000u, 0x7f800001u, {VFPU_COMPARE_MINUS_ONE, 0u, 0u}},
    {"nan-left-negative", 0xffc00000u, 0x3fc00000u, {VFPU_COMPARE_MINUS_ONE, 0u, 0u}},
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

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS

/* One deterministic 63-word grid per scalar operation: twenty shared special
   encodings followed by forty-three operation-domain values stored as raw
   binary32 words. The 11 operations produce 693 result words, packed into
   eighty-eight records of at most eight words plus one completion record. */
#define VFPU_TRANS_SPECIAL_COUNT 20u
#define VFPU_TRANS_DOMAIN_COUNT 43u
#define VFPU_TRANS_INPUT_COUNT (VFPU_TRANS_SPECIAL_COUNT + VFPU_TRANS_DOMAIN_COUNT)
#define VFPU_TRANS_CHUNK_SIZE 8u
#define VFPU_TRANS_CHUNK_COUNT ((VFPU_TRANS_INPUT_COUNT + VFPU_TRANS_CHUNK_SIZE - 1u) / \
                                VFPU_TRANS_CHUNK_SIZE)
#define VFPU_TRANS_OP_COUNT 11u
#define VFPU_TRANS_RECORD_COUNT (VFPU_TRANS_OP_COUNT * VFPU_TRANS_CHUNK_COUNT)

static const uint32_t s_vfpu_trans_specials[VFPU_TRANS_SPECIAL_COUNT] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
    0x3f800000u, 0xbf800000u, 0x3f000000u, 0xbf000000u,
    0x40000000u, 0xc0000000u, 0x00800000u, 0x80800000u,
    0x7f000000u, 0xff000000u, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xff800001u,
};

/* Replace the three VASIN specials already present in VASIN_DOMAIN_INPUTS
   with distinct in-domain values. The other operations keep the shared set. */
static const uint32_t s_vfpu_trans_asin_specials[VFPU_TRANS_SPECIAL_COUNT] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
    0x3f400000u, 0xbf400000u, 0x3f000000u, 0xbf000000u,
    0x40000000u, 0x3f600000u, 0x00800000u, 0x80800000u,
    0x7f000000u, 0xff000000u, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xff800001u,
};

/* Evenly spaced angles: -3.84375 through +4.03125 in 0.1875 increments. */
static const uint32_t s_vfpu_trans_angle_domain[VFPU_TRANS_DOMAIN_COUNT] = {
    0xc0760000u, 0xc06a0000u, 0xc05e0000u, 0xc0520000u, 0xc0460000u, 0xc03a0000u,
    0xc02e0000u, 0xc0220000u, 0xc0160000u, 0xc00a0000u, 0xbffc0000u, 0xbfe40000u,
    0xbfcc0000u, 0xbfb40000u, 0xbf9c0000u, 0xbf840000u, 0xbf580000u, 0xbf280000u,
    0xbef00000u, 0xbe900000u, 0xbdc00000u, 0x3dc00000u, 0x3e900000u, 0x3ef00000u,
    0x3f280000u, 0x3f580000u, 0x3f840000u, 0x3f9c0000u, 0x3fb40000u, 0x3fcc0000u,
    0x3fe40000u, 0x3ffc0000u, 0x400a0000u, 0x40160000u, 0x40220000u, 0x402e0000u,
    0x403a0000u, 0x40460000u, 0x40520000u, 0x405e0000u, 0x406a0000u, 0x40760000u,
    0x40810000u,
};

/* Evenly spaced powers-of-two exponents: -15.375 through +16.125 by 0.75. */
static const uint32_t s_vfpu_trans_exp_domain[VFPU_TRANS_DOMAIN_COUNT] = {
    0xc1760000u, 0xc16a0000u, 0xc15e0000u, 0xc1520000u, 0xc1460000u, 0xc13a0000u,
    0xc12e0000u, 0xc1220000u, 0xc1160000u, 0xc10a0000u, 0xc0fc0000u, 0xc0e40000u,
    0xc0cc0000u, 0xc0b40000u, 0xc09c0000u, 0xc0840000u, 0xc0580000u, 0xc0280000u,
    0xbff00000u, 0xbf900000u, 0xbec00000u, 0x3ec00000u, 0x3f900000u, 0x3ff00000u,
    0x40280000u, 0x40580000u, 0x40840000u, 0x409c0000u, 0x40b40000u, 0x40cc0000u,
    0x40e40000u, 0x40fc0000u, 0x410a0000u, 0x41160000u, 0x41220000u, 0x412e0000u,
    0x413a0000u, 0x41460000u, 0x41520000u, 0x415e0000u, 0x416a0000u, 0x41760000u,
    0x41810000u,
};

/* Evenly spaced positive log arguments: 0.0703125 through 5.3203125 by 0.125. */
static const uint32_t s_vfpu_trans_log_domain[VFPU_TRANS_DOMAIN_COUNT] = {
    0x3d900000u, 0x3e480000u, 0x3ea40000u, 0x3ee40000u, 0x3f120000u, 0x3f320000u,
    0x3f520000u, 0x3f720000u, 0x3f890000u, 0x3f990000u, 0x3fa90000u, 0x3fb90000u,
    0x3fc90000u, 0x3fd90000u, 0x3fe90000u, 0x3ff90000u, 0x40048000u, 0x400c8000u,
    0x40148000u, 0x401c8000u, 0x40248000u, 0x402c8000u, 0x40348000u, 0x403c8000u,
    0x40448000u, 0x404c8000u, 0x40548000u, 0x405c8000u, 0x40648000u, 0x406c8000u,
    0x40748000u, 0x407c8000u, 0x40824000u, 0x40864000u, 0x408a4000u, 0x408e4000u,
    0x40924000u, 0x40964000u, 0x409a4000u, 0x409e4000u, 0x40a24000u, 0x40a64000u,
    0x40aa4000u,
};

/* Evenly spaced positive square-root/reciprocal-root inputs share log's domain. */
static const uint32_t s_vfpu_trans_root_domain[VFPU_TRANS_DOMAIN_COUNT] = {
    0x3d900000u, 0x3e480000u, 0x3ea40000u, 0x3ee40000u, 0x3f120000u, 0x3f320000u,
    0x3f520000u, 0x3f720000u, 0x3f890000u, 0x3f990000u, 0x3fa90000u, 0x3fb90000u,
    0x3fc90000u, 0x3fd90000u, 0x3fe90000u, 0x3ff90000u, 0x40048000u, 0x400c8000u,
    0x40148000u, 0x401c8000u, 0x40248000u, 0x402c8000u, 0x40348000u, 0x403c8000u,
    0x40448000u, 0x404c8000u, 0x40548000u, 0x405c8000u, 0x40648000u, 0x406c8000u,
    0x40748000u, 0x407c8000u, 0x40824000u, 0x40864000u, 0x408a4000u, 0x408e4000u,
    0x40924000u, 0x40964000u, 0x409a4000u, 0x409e4000u, 0x40a24000u, 0x40a64000u,
    0x40aa4000u,
};

/* Evenly spaced reciprocal inputs: -2.5625 through +2.6875 by 0.125. */
static const uint32_t s_vfpu_trans_recip_domain[VFPU_TRANS_DOMAIN_COUNT] = {
    0xc0240000u, 0xc01c0000u, 0xc0140000u, 0xc00c0000u, 0xc0040000u, 0xbff80000u,
    0xbfe80000u, 0xbfd80000u, 0xbfc80000u, 0xbfb80000u, 0xbfa80000u, 0xbf980000u,
    0xbf880000u, 0xbf700000u, 0xbf500000u, 0xbf300000u, 0xbf100000u, 0xbee00000u,
    0xbea00000u, 0xbe400000u, 0xbd800000u, 0x3d800000u, 0x3e400000u, 0x3ea00000u,
    0x3ee00000u, 0x3f100000u, 0x3f300000u, 0x3f500000u, 0x3f700000u, 0x3f880000u,
    0x3f980000u, 0x3fa80000u, 0x3fb80000u, 0x3fc80000u, 0x3fd80000u, 0x3fe80000u,
    0x3ff80000u, 0x40040000u, 0x400c0000u, 0x40140000u, 0x401c0000u, 0x40240000u,
    0x402c0000u,
};

/* Evenly spaced VASIN inputs: -0.640625 through +0.671875 by 0.03125. */
static const uint32_t s_vfpu_trans_asin_domain[VFPU_TRANS_DOMAIN_COUNT] = {
    0xbf240000u, 0xbf1c0000u, 0xbf140000u, 0xbf0c0000u, 0xbf040000u, 0xbef80000u,
    0xbee80000u, 0xbed80000u, 0xbec80000u, 0xbeb80000u, 0xbea80000u, 0xbe980000u,
    0xbe880000u, 0xbe700000u, 0xbe500000u, 0xbe300000u, 0xbe100000u, 0xbde00000u,
    0xbda00000u, 0xbd400000u, 0xbc800000u, 0x3c800000u, 0x3d400000u, 0x3da00000u,
    0x3de00000u, 0x3e100000u, 0x3e300000u, 0x3e500000u, 0x3e700000u, 0x3e880000u,
    0x3e980000u, 0x3ea80000u, 0x3eb80000u, 0x3ec80000u, 0x3ed80000u, 0x3ee80000u,
    0x3ef80000u, 0x3f040000u, 0x3f0c0000u, 0x3f140000u, 0x3f1c0000u, 0x3f240000u,
    0x3f2c0000u,
};

typedef uint32_t (*vfpu_transcendental_fn)(uint32_t input);

#define VFPU_TRANS_OP(name, opcode)                                      \
    static uint32_t vfpu_trans_##name(uint32_t input) {                  \
        uint32_t result;                                                 \
        __asm__ volatile(                                                \
            "mtv %1, S000\n"                                             \
            opcode " S002, S000\n"                                       \
            "mfv %0, S002\n"                                             \
            : "=r"(result) : "r"(input) : "memory");                    \
        return result;                                                   \
    }

VFPU_TRANS_OP(vsin, "vsin.s")
VFPU_TRANS_OP(vcos, "vcos.s")
VFPU_TRANS_OP(vexp2, "vexp2.s")
VFPU_TRANS_OP(vlog2, "vlog2.s")
VFPU_TRANS_OP(vrsq, "vrsq.s")
VFPU_TRANS_OP(vrcp, "vrcp.s")
VFPU_TRANS_OP(vsqrt, "vsqrt.s")
VFPU_TRANS_OP(vasin, "vasin.s")
VFPU_TRANS_OP(vnrcp, "vnrcp.s")
VFPU_TRANS_OP(vnsin, "vnsin.s")
VFPU_TRANS_OP(vrexp2, "vrexp2.s")

#undef VFPU_TRANS_OP

struct vfpu_transcendental_op {
    const char *name;
    vfpu_transcendental_fn apply;
    const uint32_t *specials;
    const uint32_t *domain;
};

static const struct vfpu_transcendental_op s_vfpu_transcendental_ops[VFPU_TRANS_OP_COUNT] = {
    {"vsin", vfpu_trans_vsin, s_vfpu_trans_specials, s_vfpu_trans_angle_domain},
    {"vcos", vfpu_trans_vcos, s_vfpu_trans_specials, s_vfpu_trans_angle_domain},
    {"vexp2", vfpu_trans_vexp2, s_vfpu_trans_specials, s_vfpu_trans_exp_domain},
    {"vlog2", vfpu_trans_vlog2, s_vfpu_trans_specials, s_vfpu_trans_log_domain},
    {"vrsq", vfpu_trans_vrsq, s_vfpu_trans_specials, s_vfpu_trans_root_domain},
    {"vrcp", vfpu_trans_vrcp, s_vfpu_trans_specials, s_vfpu_trans_recip_domain},
    {"vsqrt", vfpu_trans_vsqrt, s_vfpu_trans_specials, s_vfpu_trans_root_domain},
    {"vasin", vfpu_trans_vasin, s_vfpu_trans_asin_specials, s_vfpu_trans_asin_domain},
    {"vnrcp", vfpu_trans_vnrcp, s_vfpu_trans_specials, s_vfpu_trans_recip_domain},
    {"vnsin", vfpu_trans_vnsin, s_vfpu_trans_specials, s_vfpu_trans_angle_domain},
    {"vrexp2", vfpu_trans_vrexp2, s_vfpu_trans_specials, s_vfpu_trans_exp_domain},
};

static uint32_t s_vfpu_transcendental_observed[VFPU_TRANS_OP_COUNT]
                                               [VFPU_TRANS_INPUT_COUNT];

_Static_assert(DEFERRED_MAX_RECORDS >= VFPU_TRANS_RECORD_COUNT + 1u,
               "VFPU transcendental records fit the deferred buffer");
_Static_assert(DEFERRED_MAX_OUT >= VFPU_TRANS_CHUNK_SIZE,
               "VFPU transcendental chunk fits each deferred record");

static void run_vfpu_transcendental_corpus(int emulated) {
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    probe_step(emulated, "vfpu-transcendental-corpus", "cells");

    for (uint32_t op = 0; op < VFPU_TRANS_OP_COUNT; op++) {
        const struct vfpu_transcendental_op *entry = &s_vfpu_transcendental_ops[op];
        for (uint32_t index = 0; index < VFPU_TRANS_INPUT_COUNT; index++) {
            const uint32_t input = index < VFPU_TRANS_SPECIAL_COUNT
                ? entry->specials[index]
                : entry->domain[index - VFPU_TRANS_SPECIAL_COUNT];
            s_vfpu_transcendental_observed[op][index] = entry->apply(input);
        }
    }

    for (uint32_t op = 0; op < VFPU_TRANS_OP_COUNT; op++) {
        const struct vfpu_transcendental_op *entry = &s_vfpu_transcendental_ops[op];
        for (uint32_t chunk = 0; chunk < VFPU_TRANS_CHUNK_COUNT; chunk++) {
            const uint32_t first = chunk * VFPU_TRANS_CHUNK_SIZE;
            uint32_t count = VFPU_TRANS_INPUT_COUNT - first;
            uint32_t out[VFPU_TRANS_CHUNK_SIZE];
            if (count > VFPU_TRANS_CHUNK_SIZE) {
                count = VFPU_TRANS_CHUNK_SIZE;
            }
            for (uint32_t word = 0; word < count; word++) {
                out[word] = s_vfpu_transcendental_observed[op][first + word];
            }
            char case_id[48];
            snprintf(case_id, sizeof(case_id), "%s-chunk-%02u", entry->name,
                     (unsigned int)chunk);
            defer_record(case_id, "PASS", first, out, count);
        }
    }
    const uint32_t done = VFPU_TRANS_RECORD_COUNT;
    defer_record("vfpu-transcendental-corpus-done", "PASS", 0u, &done, 1);
    flush_deferred(emulated, "PSP-VFPU-TRANSCENDENTAL-001");
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
    /* The terminal record carries the teardown outcome: TIMEOUT, with the last
       draw state as its result, when the GE did not reach idle. */
    const GeGuTeardown gu_down = ge_gu_teardown(init_rc, -1);
    const uint32_t done_out[1] = {20u};
    defer_record("ge-nan-done", gu_down.idle ? "PASS" : "TIMEOUT",
                 (uint32_t)gu_down.draw_state, done_out, 1);
    flush_deferred(emulated, "PSP-GE-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_TIMING
/* GE execution time and CPU-visible completion, for the GE timing model of #1296.
 *
 * Each cell builds its display list with libpspgu in GU_SEND mode (built, not
 * enqueued), runs an untimed setup list for its GE state, then enqueues the
 * work list GT_REPS times with no stall address and the probe's own callback
 * ID.  A rep timestamps on the 1 us system timer, from just before
 * sceGeListEnQueue:
 *   - enq:  sceGeListEnQueue returned;
 *   - fin:  the finish callback ran (the callback stores its own time);
 *   - idle: the first sceGeDrawSync(1) poll that read PSP_GE_LIST_DONE.
 * The thread polls without sleeping until it has both or GT_DEADLINE_US has
 * passed.  A timeout resets the queues and is counted in the record.
 *
 * Records are emitted after each cell, outside every timed window: each rep
 * starts from an idle GE, so a host0 write never overlaps a measurement, and
 * a launch that stops part-way keeps the cells already run.
 *
 * Lists, vertices, indices, textures and the CLUT live in cached main RAM and
 * are written back before the reps; the colour and depth buffers are in eDRAM.
 * The matrix runs at 222/111 MHz and again at 333/166 MHz (CPU/bus); the boot
 * clocks are restored after the last pass.  Nothing here touches retail code
 * or data. */
#define GT_REPS 8u
#define GT_EMPTY_REPS 32u
#define GT_C_REPS 8u
#define GT_MAX_REPS 32u
#define GT_DEADLINE_US 250000u
#define GT_W 480
#define GT_H 272
#define GT_STRIDE 512
#define GT_FBP ((void *)0)
#define GT_ZBP ((void *)0x88000)
#define GT_VRAM_DST ((void *)0x04000000)
#define GT_VRAM_SRC ((void *)0x04100000)  /* 512 x 272 x 4 bytes, above the depth buffer */
#define GT_LIST_WORDS (128u * 1024u)
#define GT_SMALL_WORDS 8192u
#define GT_VTX_BYTES (1024u * 1024u)
#define GT_MAX_VERTS 18432u
#define GT_GRID_W 384  /* sprite grid: 384 x 256 px, divisible by 8, 32 and 128 */
#define GT_GRID_H 256
#define GT_LONG_PASSES 16u
#define GT_SENTINEL 0x12345678u
#define GT_COLOR_A 0xff00ff00u
#define GT_COLOR_B 0xffff0000u
#define GT_BEHAVIOUR_CONTINUE 2u  /* SIGNAL behaviour in bits 16..23 */
#define GT_SIGNAL_ID 0x5a5u

_Static_assert(GT_MAX_VERTS * 44u <= GT_VTX_BYTES,
               "eight float weights plus a float position must fit the vertex buffer");
_Static_assert(16384u * 3u * 12u <= GT_VTX_BYTES, "t6 one-pixel triangles must fit the vertex buffer");

typedef struct { uint32_t color; int16_t x, y, z, pad; } GtFlatV;     /* stride 12 */
typedef struct { int16_t u, v, x, y, z; } GtTexV;                    /* stride 10 */
typedef struct { uint32_t color; float x, y, z; } GtXfV;             /* stride 16 */

static uint32_t s_gt_list[GT_LIST_WORDS] __attribute__((aligned(64)));
static uint32_t s_gt_list_b[GT_SMALL_WORDS] __attribute__((aligned(64)));
static uint32_t s_gt_setup[GT_SMALL_WORDS] __attribute__((aligned(64)));
static uint8_t s_gt_vtx[GT_VTX_BYTES] __attribute__((aligned(64)));
static GtFlatV s_gt_strips[16] __attribute__((aligned(64)));
static GtFlatV s_gt_grid_b[2u * (GT_GRID_W / 8) * (GT_GRID_H / 8)] __attribute__((aligned(64)));
static GtTexV s_gt_tex_strips[16] __attribute__((aligned(64)));
static GtXfV s_gt_xf_strips[16] __attribute__((aligned(64)));
static uint16_t s_gt_idx[GT_MAX_VERTS] __attribute__((aligned(64)));
static uint32_t s_gt_tex[512u * 512u] __attribute__((aligned(64)));
static uint32_t s_gt_clut[256] __attribute__((aligned(64)));
static const ScePspFMatrix4 s_gt_identity = {
    {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f},
};

static int s_gt_cbid = -1;
static uint32_t s_gt_records;  /* PSP-GE-TIMING-001 records emitted before ge-timing-done */
static volatile uint32_t s_gt_fin_count;
static volatile uint32_t s_gt_fin_time[4];
static volatile uint32_t s_gt_fin_id[4];
static volatile uint32_t s_gt_sig_count;
static volatile uint32_t s_gt_sig_time;
static volatile uint32_t s_gt_sig_id;
static uint32_t s_gt_last_t0;  /* t0 of the latest gt_run */
static uint32_t s_gt_cpu_mhz;
static uint32_t s_gt_bus_mhz;
static char s_gt_clk[8];

static void gt_finish_cb(int id, void *arg) {
    (void)arg;
    const uint32_t n = s_gt_fin_count;
    if (n < 4u) {
        s_gt_fin_time[n] = sceKernelGetSystemTimeLow();
        s_gt_fin_id[n] = (uint32_t)id;
    }
    s_gt_fin_count = n + 1u;
}

static void gt_signal_cb(int id, void *arg) {
    (void)arg;
    if (!s_gt_sig_count) {
        s_gt_sig_time = sceKernelGetSystemTimeLow();
        s_gt_sig_id = (uint32_t)id;
    }
    s_gt_sig_count++;
}

typedef struct {
    uint32_t fin[GT_MAX_REPS], idle[GT_MAX_REPS], enq[GT_MAX_REPS];
    uint32_t fin2[GT_MAX_REPS];  /* second list (t15) */
    uint32_t n, timeouts, errors;
    int last_error;
} GtStats;

/* Run lists[0..count) back to back once and wait, bounded, for every finish
   callback and an idle GE.  Returns 1 on a timeout, -1 on an enqueue error. */
static int gt_run(void *const *lists, uint32_t count, uint32_t *fin, uint32_t *fin2,
                  uint32_t *idle, uint32_t *enq, int *err) {
    s_gt_fin_count = 0;
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    s_gt_last_t0 = t0;
    for (uint32_t i = 0; i < count; i++) {
        const int qid = sceGeListEnQueue(lists[i], NULL, s_gt_cbid, NULL);
        if (qid < 0) {
            *err = qid;
            GeIdleWait after;
            (void)ge_reset_queues(&after);
            return -1;
        }
    }
    const uint32_t t1 = sceKernelGetSystemTimeLow();
    uint32_t t_idle = 0;
    for (;;) {
        const int ds = sceGeDrawSync(1);
        const uint32_t t = sceKernelGetSystemTimeLow();
        if (!t_idle && ds == PSP_GE_LIST_DONE) t_idle = t;
        if (t_idle && s_gt_fin_count >= count) break;
        if (t - t0 >= GT_DEADLINE_US) {
            GeIdleWait after;
            (void)ge_reset_queues(&after);
            return 1;
        }
    }
    *fin = s_gt_fin_time[0] - t0;
    if (fin2) *fin2 = count > 1u ? s_gt_fin_time[1] - t0 : 0u;
    *idle = t_idle - t0;
    *enq = t1 - t0;
    return 0;
}

static void gt_measure(void *const *lists, uint32_t count, uint32_t reps, GtStats *st) {
    memset(st, 0, sizeof(*st));
    sceKernelDcacheWritebackAll();
    for (uint32_t r = 0; r < reps; r++) {
        uint32_t fin = 0, fin2 = 0, idle = 0, enq = 0;
        int err = 0;
        const int rc = gt_run(lists, count, &fin, &fin2, &idle, &enq, &err);
        if (rc > 0) { st->timeouts++; continue; }
        if (rc < 0) { st->errors++; st->last_error = err; continue; }
        st->fin[st->n] = fin;
        st->fin2[st->n] = fin2;
        st->idle[st->n] = idle;
        st->enq[st->n] = enq;
        st->n++;
    }
}

static void gt_sort(uint32_t *v, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t x = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
}

/* min, median (mean of the middle pair for an even count) and max, in place. */
static void gt_summary(uint32_t *v, uint32_t n, uint32_t out[3]) {
    if (!n) { out[0] = out[1] = out[2] = 0xffffffffu; return; }
    gt_sort(v, n);
    out[0] = v[0];
    out[1] = (n & 1u) ? v[n / 2u] : (v[n / 2u - 1u] + v[n / 2u] + 1u) / 2u;
    out[2] = v[n - 1u];
}

/* Record layout (PSP-GE-TIMING-001 timing cells):
   out0 size parameter, out1 work units (words, vertices, pixels, bytes or
   entries; the case README names the unit per cell), out2 list bytes, out3
   reps completed, out4 timeouts, out5 enqueue errors, out6-8 fin min/med/max,
   out9-11 idle min/med/max, out12 enqueue med, out13 CPU MHz, out14 bus MHz,
   out15 last enqueue error.  All times are microseconds from just before the
   first enqueue.  The result word is the fin median. */
/* Every PSP-GE-TIMING-001 record goes through here, so the ge-timing-done
   completion record can carry the number emitted before it. */
static void gt_record(int emulated, const char *case_id, const char *status,
                      uint32_t result, const uint32_t *out, size_t count) {
    emit_record_extended(emulated, "PSP-GE-TIMING-001", case_id, status, result, out, count);
    s_gt_records++;
}

static void gt_emit(int emulated, const char *cell, uint32_t size, uint32_t units,
                    uint32_t list_bytes, GtStats *st) {
    uint32_t fin[3], idle[3], enq[3];
    gt_summary(st->fin, st->n, fin);
    gt_summary(st->idle, st->n, idle);
    gt_summary(st->enq, st->n, enq);
    const uint32_t out[16] = {
        size, units, list_bytes, st->n, st->timeouts, st->errors,
        fin[0], fin[1], fin[2], idle[0], idle[1], idle[2], enq[1],
        s_gt_cpu_mhz, s_gt_bus_mhz, (uint32_t)st->last_error,
    };
    char case_id[64];
    snprintf(case_id, sizeof(case_id), "gt-%s-%s-%u", s_gt_clk, cell, (unsigned int)size);
    gt_record(emulated, case_id, st->errors ? "FAIL" : st->timeouts ? "TIMEOUT" : "PASS",
              fin[1], out, 16);
}

static void gt_begin(uint32_t *list) { (void)sceGuStart(GU_SEND, list); }
static uint32_t gt_end(void) { return (uint32_t)sceGuFinish(); }

/* The state every cell starts from: 8888 (or 5650) colour buffer at eDRAM 0,
   16-bit depth buffer after it, full-screen viewport and scissor, identity
   matrices, and every per-pixel test, texturing and lighting off. */
static void gt_base_state(int psm) {
    sceGuDrawBuffer(psm, GT_FBP, GT_STRIDE);
    sceGuDepthBuffer(GT_ZBP, GT_STRIDE);
    sceGuOffset(2048 - (GT_W / 2), 2048 - (GT_H / 2));
    sceGuViewport(2048, 2048, GT_W, GT_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, GT_W, GT_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_LIGHTING);
    for (int l = 0; l < 4; l++) sceGuDisable(GU_LIGHT0 + l);
    sceGuDepthMask(GU_TRUE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuSetMatrix(GU_PROJECTION, &s_gt_identity);
    sceGuSetMatrix(GU_VIEW, &s_gt_identity);
    sceGuSetMatrix(GU_MODEL, &s_gt_identity);
    sceGuColor(0xffffffffu);
    sceGuSendCommandi(211, 0);  /* clear mode off */
}

/* Run the setup list once, untimed; a failure is folded into the cell's record. */
static void gt_apply_setup(GtStats *setup) {
    void *lists[1] = {s_gt_setup};
    gt_measure(lists, 1, 1, setup);
}

/* Through-mode 64-px-wide strips covering the screen (8 sprites, 16 vertices). */
static void gt_prepare_strips(void) {
    for (int k = 0; k < 8; k++) {
        const int16_t x0 = (int16_t)(k * 64), x1 = (int16_t)(k == 7 ? GT_W : (k + 1) * 64);
        s_gt_strips[2 * k] = (GtFlatV){0x80ffffffu, x0, 0, 0x7fff, 0};
        s_gt_strips[2 * k + 1] = (GtFlatV){0x80ffffffu, x1, GT_H, 0x7fff, 0};
        s_gt_tex_strips[2 * k] = (GtTexV){x0, 0, x0, 0, 0};
        s_gt_tex_strips[2 * k + 1] = (GtTexV){x1, GT_H, x1, GT_H, 0};
        const float fx0 = -1.0f + 2.0f * (float)x0 / (float)GT_W;
        const float fx1 = -1.0f + 2.0f * (float)x1 / (float)GT_W;
        s_gt_xf_strips[2 * k] = (GtXfV){0xffffffffu, fx0, 1.0f, 0.0f};
        s_gt_xf_strips[2 * k + 1] = (GtXfV){0xffffffffu, fx1, -1.0f, 0.0f};
    }
}

/* Sprites of edge s px tiling the 384 x 256 grid; returns the vertex count. */
static uint32_t gt_prepare_grid(GtFlatV *v, int s, uint32_t color) {
    uint32_t n = 0;
    for (int y = 0; y < GT_GRID_H; y += s) {
        for (int x = 0; x < GT_GRID_W; x += s) {
            v[n++] = (GtFlatV){color, (int16_t)x, (int16_t)y, 0, 0};
            v[n++] = (GtFlatV){color, (int16_t)(x + s), (int16_t)(y + s), 0, 0};
        }
    }
    return n;
}

static uint32_t gt_align(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
static uint32_t gt_fmt_size(uint32_t fmt) { return fmt == 1u ? 1u : fmt == 2u ? 2u : fmt == 3u ? 4u : 0u; }

static void gt_put(uint8_t *p, uint32_t size, float value) {
    if (size == 4u) { memcpy(p, &value, 4); return; }
    if (size == 2u) { const int16_t q = (int16_t)(value * 32767.0f + (value >= 0 ? 0.5f : -0.5f)); memcpy(p, &q, 2); return; }
    *(int8_t *)p = (int8_t)(value * 127.0f + (value >= 0 ? 0.5f : -0.5f));
}

/* Pack `count` transformed vertices of type vtype at the screen centre for
   tiny triangles: vertex i sits at x = (i & 1) * d, y = ((i >> 1) & 1) * d, so
   any three consecutive vertices span a non-degenerate triangle of about two
   pixels, in a list or a strip.  d is 1.5 px, or one quantum for 8-bit
   positions.  Weights are 1/n, texture coordinates 0, colour opaque white,
   normals +z.  Field order and alignment follow the GE vertex layout. */
static uint32_t gt_pack_tiny(uint32_t vtype, uint32_t count) {
    const uint32_t tf = vtype & 3u, cf = (vtype >> 2) & 7u, nf = (vtype >> 5) & 3u;
    const uint32_t pf = (vtype >> 7) & 3u, wf = (vtype >> 9) & 3u;
    const uint32_t nw = wf ? ((vtype >> 14) & 7u) + 1u : 0u;
    const uint32_t ts = gt_fmt_size(tf), ns = gt_fmt_size(nf), ps = gt_fmt_size(pf), ws = gt_fmt_size(wf);
    const uint32_t cs = cf == 7u ? 4u : cf ? 2u : 0u;
    uint32_t off = 0, align = 1;
    uint32_t ow = 0, ot = 0, oc = 0, on = 0, op = 0;
    if (ws) { off = gt_align(off, ws); ow = off; off += nw * ws; if (ws > align) align = ws; }
    if (ts) { off = gt_align(off, ts); ot = off; off += 2u * ts; if (ts > align) align = ts; }
    if (cs) { off = gt_align(off, cs); oc = off; off += cs; if (cs > align) align = cs; }
    if (ns) { off = gt_align(off, ns); on = off; off += 3u * ns; if (ns > align) align = ns; }
    off = gt_align(off, ps); op = off; off += 3u * ps; if (ps > align) align = ps;
    const uint32_t stride = gt_align(off, align);
    if (stride * count > GT_VTX_BYTES) return 0;
    const float d = ps == 1u ? 1.0f / 127.0f : 3.0f / (float)GT_W;
    memset(s_gt_vtx, 0, stride * count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *v = s_gt_vtx + i * stride;
        for (uint32_t w = 0; w < nw; w++) gt_put(v + ow + w * ws, ws, 1.0f / (float)nw);
        if (ts) { gt_put(v + ot, ts, 0.0f); gt_put(v + ot + ts, ts, 0.0f); }
        if (cs == 4u) memset(v + oc, 0xff, 4);
        if (ns) { gt_put(v + on, ns, 0.0f); gt_put(v + on + ns, ns, 0.0f); gt_put(v + on + 2u * ns, ns, 1.0f); }
        gt_put(v + op, ps, (i & 1u) ? d : 0.0f);
        gt_put(v + op + ps, ps, ((i >> 1) & 1u) ? d : 0.0f);
        gt_put(v + op + 2u * ps, ps, 0.0f);
    }
    return stride;
}

/* One-PRIM timing cell over the shared vertex buffer: setup, build, measure, emit. */
static void gt_cell_draw(int emulated, const char *cell, uint32_t size, uint32_t units,
                         int prim, uint32_t vtype, uint32_t count, const void *indices) {
    GtStats setup, st;
    gt_apply_setup(&setup);
    gt_begin(s_gt_list);
    sceGuDrawArray(prim, (int)vtype, (int)count, indices, s_gt_vtx);
    const uint32_t bytes = gt_end();
    void *lists[1] = {s_gt_list};
    gt_measure(lists, 1, GT_REPS, &st);
    st.timeouts += setup.timeouts;
    st.errors += setup.errors;
    gt_emit(emulated, cell, size, units, bytes, &st);
}

static void gt_setup_base(int psm) {
    gt_begin(s_gt_setup);
    gt_base_state(psm);
    (void)gt_end();
}

static void gt_measure_built(int emulated, const char *cell, uint32_t size, uint32_t units,
                             uint32_t bytes, uint32_t reps) {
    GtStats setup, st;
    gt_apply_setup(&setup);
    void *lists[1] = {s_gt_list};
    gt_measure(lists, 1, reps, &st);
    st.timeouts += setup.timeouts;
    st.errors += setup.errors;
    gt_emit(emulated, cell, size, units, bytes, &st);
}

static const uint32_t s_gt_word_sizes[3] = {1024u, 4096u, 16384u};
static const uint32_t s_gt_vert_sizes[3] = {1536u, 6144u, 18432u};
static const uint32_t s_gt_counts[3] = {1u, 4u, 16u};

/* T0-T3: list overhead and non-draw command runs. */
static void gt_cells_commands(int emulated) {
    probe_step(emulated, "ge-timing", "t0-t3");
    gt_setup_base(GU_PSM_8888);
    gt_begin(s_gt_list);
    gt_measure_built(emulated, "t0-empty", 0u, 0u, gt_end(), GT_EMPTY_REPS);
    for (int s = 0; s < 3; s++) {
        const uint32_t n = s_gt_word_sizes[s];
        gt_begin(s_gt_list);
        for (uint32_t i = 0; i < n; i++) sceGuSendCommandi(0, 0);
        gt_measure_built(emulated, "t1-nop", n, n, gt_end(), GT_REPS);
        /* Material ambient, diffuse, specular and the ambient light colour:
           state writes that start no GE work. */
        static const int state_cmds[4] = {0x55, 0x56, 0x57, 0x5c};
        gt_begin(s_gt_list);
        for (uint32_t i = 0; i < n; i++) sceGuSendCommandi(state_cmds[i & 3u], 0x808080);
        gt_measure_built(emulated, "t2-state", n, n, gt_end(), GT_REPS);
        /* World-matrix uploads: one select word and twelve data words each. */
        gt_begin(s_gt_list);
        const uint32_t uploads = n / 13u;
        for (uint32_t i = 0; i < uploads; i++) sceGuSetMatrix(GU_MODEL, &s_gt_identity);
        gt_measure_built(emulated, "t3-matrix", n, uploads * 13u, gt_end(), GT_REPS);
    }
}

/* T4: clear-mode full-screen strips, colour or colour+depth, 8888 or 5650. */
static void gt_cells_clear(int emulated) {
    probe_step(emulated, "ge-timing", "t4");
    static const struct { const char *name; int psm; int flags; } variants[4] = {
        {"t4-clear-c8888", GU_PSM_8888, GU_COLOR_BUFFER_BIT},
        {"t4-clear-cz8888", GU_PSM_8888, GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT},
        {"t4-clear-c5650", GU_PSM_5650, GU_COLOR_BUFFER_BIT},
        {"t4-clear-cz5650", GU_PSM_5650, GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT},
    };
    for (int v = 0; v < 4; v++) {
        for (int s = 0; s < 3; s++) {
            const uint32_t count = s_gt_counts[s];
            gt_setup_base(variants[v].psm);
            gt_begin(s_gt_list);
            for (uint32_t k = 0; k < count; k++) {
                sceGuSendCommandi(211, (variants[v].flags << 8) | 1);
                sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                               16, NULL, s_gt_strips);
                sceGuSendCommandi(211, 0);
            }
            gt_measure_built(emulated, variants[v].name, count, count * GT_W * GT_H, gt_end(), GT_REPS);
        }
    }
}

/* T5/T6: through-mode flat sprites and one-pixel triangles, one PRIM or one per primitive. */
static void gt_cells_through(int emulated) {
    probe_step(emulated, "ge-timing", "t5-t6");
    static const int edges[3] = {8, 32, 128};
    const uint32_t vt = GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D;
    for (int s = 0; s < 3; s++) {
        const uint32_t n = gt_prepare_grid((GtFlatV *)s_gt_vtx, edges[s], GT_COLOR_A);
        gt_setup_base(GU_PSM_8888);
        gt_cell_draw(emulated, "t5-sprite", (uint32_t)edges[s], GT_GRID_W * GT_GRID_H,
                     GU_SPRITES, vt, n, NULL);
        gt_setup_base(GU_PSM_8888);
        gt_begin(s_gt_list);
        for (uint32_t i = 0; i < n; i += 2u)
            sceGuDrawArray(GU_SPRITES, (int)vt, 2, NULL, (GtFlatV *)s_gt_vtx + i);
        gt_measure_built(emulated, "t5-sprite-perprim", (uint32_t)edges[s], GT_GRID_W * GT_GRID_H,
                         gt_end(), GT_REPS);
    }
    for (int s = 0; s < 3; s++) {
        const uint32_t tris = s_gt_word_sizes[s];
        GtFlatV *v = (GtFlatV *)s_gt_vtx;
        for (uint32_t i = 0; i < tris; i++) {
            v[3 * i] = (GtFlatV){GT_COLOR_A, 240, 136, 0, 0};
            v[3 * i + 1] = (GtFlatV){GT_COLOR_A, 241, 136, 0, 0};
            v[3 * i + 2] = (GtFlatV){GT_COLOR_A, 240, 137, 0, 0};
        }
        gt_setup_base(GU_PSM_8888);
        gt_cell_draw(emulated, "t6-tri1px", tris, tris, GU_TRIANGLES, vt, 3u * tris, NULL);
        gt_setup_base(GU_PSM_8888);
        gt_begin(s_gt_list);
        for (uint32_t i = 0; i < tris; i++)
            sceGuDrawArray(GU_TRIANGLES, (int)vt, 3, NULL, v + 3u * i);
        gt_measure_built(emulated, "t6-tri1px-perprim", tris, tris, gt_end(), GT_REPS);
    }
}

/* T7-T10: transformed tiny triangles by primitive form, vertex format, lights, weights. */
static void gt_cells_transform(int emulated) {
    probe_step(emulated, "ge-timing", "t7-t10");
    const uint32_t xf = GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    for (uint32_t i = 0; i < GT_MAX_VERTS; i++) s_gt_idx[i] = (uint16_t)i;
    static const struct { const char *name; uint32_t vtype; } formats[6] = {
        {"t8-pos16", GU_VERTEX_16BIT | GU_TRANSFORM_3D},
        {"t8-pos8", GU_VERTEX_8BIT | GU_TRANSFORM_3D},
        {"t8-color", GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D},
        {"t8-uv", GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D},
        {"t8-normal", GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D},
        {"t8-full", GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF |
                        GU_TRANSFORM_3D},
    };
    for (int s = 0; s < 3; s++) {
        const uint32_t n = s_gt_vert_sizes[s];
        (void)gt_pack_tiny(xf, n);
        gt_setup_base(GU_PSM_8888);
        gt_cell_draw(emulated, "t7-list", n, n, GU_TRIANGLES, xf, n, NULL);
        gt_setup_base(GU_PSM_8888);
        gt_cell_draw(emulated, "t7-strip", n, n, GU_TRIANGLE_STRIP, xf, n, NULL);
        gt_setup_base(GU_PSM_8888);
        gt_cell_draw(emulated, "t7-indexed", n, n, GU_TRIANGLES, xf | GU_INDEX_16BIT, n, s_gt_idx);
        for (int f = 0; f < 6; f++) {
            (void)gt_pack_tiny(formats[f].vtype, n);
            gt_setup_base(GU_PSM_8888);
            gt_cell_draw(emulated, formats[f].name, n, n, GU_TRIANGLES, formats[f].vtype, n, NULL);
        }
        static const int lights[3] = {0, 1, 4};
        const uint32_t lit = GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
        (void)gt_pack_tiny(lit, n);
        for (int l = 0; l < 3; l++) {
            gt_begin(s_gt_setup);
            gt_base_state(GU_PSM_8888);
            sceGuEnable(GU_LIGHTING);
            sceGuAmbient(0xff202020u);
            for (int k = 0; k < lights[l]; k++) {
                const ScePspFVector3 dir = {-0.25f, -0.5f, -1.0f};
                sceGuEnable(GU_LIGHT0 + k);
                sceGuLight(k, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
                sceGuLightColor(k, GU_DIFFUSE, 0xffffffffu);
            }
            (void)gt_end();
            char name[24];
            snprintf(name, sizeof(name), "t9-lights%d", lights[l]);
            gt_cell_draw(emulated, name, n, n, GU_TRIANGLES, lit, n, NULL);
        }
        static const uint32_t weights[3] = {1u, 4u, 8u};
        for (int w = 0; w < 3; w++) {
            const uint32_t vt = GU_WEIGHTS(weights[w]) | GU_WEIGHT_32BITF | xf;
            (void)gt_pack_tiny(vt, n);
            gt_begin(s_gt_setup);
            gt_base_state(GU_PSM_8888);
            for (uint32_t b = 0; b < weights[w]; b++) sceGuBoneMatrix(b, &s_gt_identity);
            (void)gt_end();
            char name[24];
            snprintf(name, sizeof(name), "t10-weights%u", (unsigned int)weights[w]);
            gt_cell_draw(emulated, name, n, n, GU_TRIANGLES, vt, n, NULL);
        }
    }
}

/* T11: textured full-screen strips by format, swizzle, texture size and filter. */
static void gt_cells_texture(int emulated) {
    probe_step(emulated, "ge-timing", "t11");
    static const struct { const char *name; int psm; int swizzle; } formats[9] = {
        {"5650", GU_PSM_5650, 0}, {"5650s", GU_PSM_5650, 1},
        {"8888", GU_PSM_8888, 0}, {"8888s", GU_PSM_8888, 1},
        {"t4", GU_PSM_T4, 0}, {"t4s", GU_PSM_T4, 1},
        {"t8", GU_PSM_T8, 0}, {"t8s", GU_PSM_T8, 1},
        {"dxt1", GU_PSM_DXT1, 0},
    };
    static const int sizes[2] = {64, 512};
    for (int f = 0; f < 9; f++) {
        for (int z = 0; z < 2; z++) {
            for (int filt = 0; filt < 2; filt++) {
                for (int s = 0; s < 3; s++) {
                    const uint32_t count = s_gt_counts[s];
                    gt_begin(s_gt_setup);
                    gt_base_state(GU_PSM_8888);
                    sceGuEnable(GU_TEXTURE_2D);
                    sceGuTexMode(formats[f].psm, 0, 0, formats[f].swizzle);
                    sceGuTexImage(0, sizes[z], sizes[z], sizes[z], s_gt_tex);
                    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
                    sceGuTexFilter(filt ? GU_LINEAR : GU_NEAREST, filt ? GU_LINEAR : GU_NEAREST);
                    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
                    if (formats[f].psm == GU_PSM_T4 || formats[f].psm == GU_PSM_T8) {
                        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
                        sceGuClutLoad(formats[f].psm == GU_PSM_T4 ? 2 : 32, s_gt_clut);
                    }
                    sceGuTexFlush();
                    (void)gt_end();
                    gt_begin(s_gt_list);
                    for (uint32_t k = 0; k < count; k++)
                        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                                       16, NULL, s_gt_tex_strips);
                    char name[40];
                    snprintf(name, sizeof(name), "t11-%s-%d-%s", formats[f].name, sizes[z],
                             filt ? "linear" : "nearest");
                    gt_measure_built(emulated, name, count, count * GT_W * GT_H, gt_end(), GT_REPS);
                }
            }
        }
    }
}

/* T12: per-pixel state on full-screen strips. */
static void gt_cells_fill(int emulated) {
    probe_step(emulated, "ge-timing", "t12");
    for (int v = 0; v < 7; v++) {
        static const char *const names[7] = {
            "t12-base", "t12-blend", "t12-alphatest", "t12-depth", "t12-dither",
            "t12-xfbase", "t12-fog",
        };
        for (int s = 0; s < 3; s++) {
            const uint32_t count = s_gt_counts[s];
            gt_begin(s_gt_setup);
            gt_base_state(GU_PSM_8888);
            if (v == 1) {
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            } else if (v == 2) {
                sceGuEnable(GU_ALPHA_TEST);
                sceGuAlphaFunc(GU_GREATER, 0x10, 0xff);
            } else if (v == 3) {
                /* Depth 0 everywhere first, so GEQUAL against z 0x7fff always passes and writes. */
                sceGuSendCommandi(211, (GU_DEPTH_BUFFER_BIT << 8) | 1);
                static GtFlatV zero[16];
                for (int i = 0; i < 16; i++) { zero[i] = s_gt_strips[i]; zero[i].z = 0; }
                sceKernelDcacheWritebackRange(zero, sizeof(zero));
                sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                               16, NULL, zero);
                sceGuSendCommandi(211, 0);
                sceGuEnable(GU_DEPTH_TEST);
                sceGuDepthFunc(GU_GEQUAL);
                sceGuDepthMask(GU_FALSE);
            } else if (v == 4) {
                sceGuEnable(GU_DITHER);
            } else if (v == 6) {
                sceGuEnable(GU_FOG);
                sceGuFog(0.0f, 1.0f, 0xff808080u);
            }
            (void)gt_end();
            gt_begin(s_gt_list);
            for (uint32_t k = 0; k < count; k++) {
                if (v >= 5)
                    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                                   16, NULL, s_gt_xf_strips);
                else
                    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                                   16, NULL, s_gt_strips);
            }
            gt_measure_built(emulated, names[v], count, count * GT_W * GT_H, gt_end(), GT_REPS);
        }
    }
}

/* T13/T14: CLUT loads and block transfers. */
static void gt_cells_upload(int emulated) {
    probe_step(emulated, "ge-timing", "t13-t14");
    static const uint32_t loads[3] = {64u, 256u, 1024u};
    for (int e = 0; e < 2; e++) {
        const int blocks = e ? 32 : 2;
        for (int s = 0; s < 3; s++) {
            gt_begin(s_gt_setup);
            gt_base_state(GU_PSM_8888);
            sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
            (void)gt_end();
            gt_begin(s_gt_list);
            for (uint32_t i = 0; i < loads[s]; i++) sceGuClutLoad(blocks, s_gt_clut);
            gt_measure_built(emulated, e ? "t13-clut256" : "t13-clut16", loads[s],
                             loads[s] * (uint32_t)blocks * 8u, gt_end(), GT_REPS);
        }
    }
    static const int w[3] = {64, 256, 480}, h[3] = {64, 256, 272};
    for (int src = 0; src < 2; src++) {
        for (int s = 0; s < 3; s++) {
            gt_setup_base(GU_PSM_8888);
            gt_begin(s_gt_list);
            sceGuCopyImage(GU_PSM_8888, 0, 0, w[s], h[s], 512, src ? GT_VRAM_SRC : (void *)s_gt_tex,
                           0, 0, 512, GT_VRAM_DST);
            gt_measure_built(emulated, src ? "t14-copy-vram" : "t14-copy-ram", (uint32_t)w[s],
                             (uint32_t)(w[s] * h[s] * 4), gt_end(), GT_REPS);
        }
    }
}

/* T15: two T5 32-px lists enqueued back to back.  out6-8 are the first list's
   finish, out9-11 the second's (in place of idle), out12 the idle median. */
static void gt_cell_two_lists(int emulated) {
    probe_step(emulated, "ge-timing", "t15");
    const uint32_t n = gt_prepare_grid((GtFlatV *)s_gt_vtx, 32, GT_COLOR_A);
    const uint32_t vt = GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D;
    gt_setup_base(GU_PSM_8888);
    GtStats setup, st;
    gt_apply_setup(&setup);
    gt_begin(s_gt_list);
    sceGuDrawArray(GU_SPRITES, (int)vt, (int)n, NULL, s_gt_vtx);
    const uint32_t bytes = gt_end();
    gt_begin(s_gt_list_b);
    sceGuDrawArray(GU_SPRITES, (int)vt, (int)n, NULL, s_gt_vtx);
    (void)gt_end();
    void *lists[2] = {s_gt_list, s_gt_list_b};
    gt_measure(lists, 2, GT_REPS, &st);
    uint32_t fin[3], fin2[3], idle[3];
    gt_summary(st.fin, st.n, fin);
    gt_summary(st.fin2, st.n, fin2);
    gt_summary(st.idle, st.n, idle);
    const uint32_t out[16] = {
        32u, 2u * GT_GRID_W * GT_GRID_H, bytes, st.n, st.timeouts + setup.timeouts,
        st.errors + setup.errors, fin[0], fin[1], fin[2], fin2[0], fin2[1], fin2[2], idle[1],
        s_gt_cpu_mhz, s_gt_bus_mhz, (uint32_t)st.last_error,
    };
    char case_id[64];
    snprintf(case_id, sizeof(case_id), "gt-%s-t15-twolists-32", s_gt_clk);
    gt_record(emulated, case_id, st.errors ? "FAIL" : st.timeouts ? "TIMEOUT" : "PASS",
              fin2[1], out, 16);
}

/* The long list the completion cells share: GT_LONG_PASSES passes over the
   8-px sprite grid, the last pass in a second colour so its final pixel can be
   told from the earlier passes'.  Returns the list bytes. */
static uint32_t gt_build_long(int with_signal) {
    const uint32_t n = gt_prepare_grid((GtFlatV *)s_gt_vtx, 8, GT_COLOR_A);
    (void)gt_prepare_grid(s_gt_grid_b, 8, GT_COLOR_B);
    const uint32_t vt = GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D;
    gt_begin(s_gt_list);
    for (uint32_t p = 0; p < GT_LONG_PASSES; p++) {
        if (with_signal && p == GT_LONG_PASSES / 2u) {
            sceGuSendCommandi(14, (int)((GT_BEHAVIOUR_CONTINUE << 16) | GT_SIGNAL_ID));
            sceGuSendCommandi(12, 0);
        }
        sceGuDrawArray(GU_SPRITES, (int)vt, (int)n, NULL,
                       p + 1u == GT_LONG_PASSES ? (void *)s_gt_grid_b : (void *)s_gt_vtx);
    }
    return gt_end();
}

static void gt_emit_rep(int emulated, const char *cell, uint32_t rep, const char *status,
                        uint32_t result, const uint32_t *out, size_t count) {
    char case_id[64];
    snprintf(case_id, sizeof(case_id), "gt-%s-%s-r%u", s_gt_clk, cell, (unsigned int)rep);
    gt_record(emulated, case_id, status, result, out, count);
}

/* C1+C3: on the long list, the time the last pass's final pixel reaches eDRAM
   (read through the uncached mirror), the finish callback, and the
   sceGeListSync(qid, 1) / sceGeDrawSync(1) values from the enqueue to idle.
   out0/1 first list/draw state, out2-5 the next two list-state changes
   (value, us), out6-9 the next two draw-state changes, out10 pixel us, out11
   fin us, out12/13 list/draw state at the first poll after the callback ran,
   out14 enqueue us, out15 timed out, out16 list bytes. */
static void gt_cell_peeks(int emulated, uint32_t bytes) {
    probe_step(emulated, "ge-timing", "c1-c3");
    volatile uint32_t *pix = (volatile uint32_t *)(0x44000000u +
        (uint32_t)(((GT_GRID_H - 1) * GT_STRIDE + (GT_GRID_W - 1)) * 4));
    for (uint32_t r = 0; r < GT_C_REPS; r++) {
        uint32_t out[17];
        memset(out, 0, sizeof(out));
        out[16] = bytes;
        *pix = GT_SENTINEL;
        sceKernelDcacheWritebackAll();
        s_gt_fin_count = 0;
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const int qid = sceGeListEnQueue(s_gt_list, NULL, s_gt_cbid, NULL);
        out[14] = sceKernelGetSystemTimeLow() - t0;
        if (qid < 0) {
            out[0] = (uint32_t)qid;
            gt_emit_rep(emulated, "c13-peeks", r, "FAIL", (uint32_t)qid, out, 17);
            continue;
        }
        int pl = sceGeListSync(qid, 1), pd = sceGeDrawSync(1);
        out[0] = (uint32_t)pl;
        out[1] = (uint32_t)pd;
        uint32_t nl = 0, nd = 0, seen_fin = 0;
        for (;;) {
            const int l = sceGeListSync(qid, 1);
            const int d = sceGeDrawSync(1);
            const uint32_t t = sceKernelGetSystemTimeLow() - t0;
            if (l != pl && nl < 2u) { out[2 + 2 * nl] = (uint32_t)l; out[3 + 2 * nl] = t; nl++; }
            if (d != pd && nd < 2u) { out[6 + 2 * nd] = (uint32_t)d; out[7 + 2 * nd] = t; nd++; }
            pl = l;
            pd = d;
            if (!out[10] && (*pix & 0x00ffffffu) == (GT_COLOR_B & 0x00ffffffu)) out[10] = t;
            if (!seen_fin && s_gt_fin_count) {
                seen_fin = 1;
                out[11] = s_gt_fin_time[0] - t0;
                out[12] = (uint32_t)l;
                out[13] = (uint32_t)d;
            }
            if (out[10] && seen_fin && d == PSP_GE_LIST_DONE) break;
            if (t >= GT_DEADLINE_US) {
                GeIdleWait after;
                out[15] = 1;
                (void)ge_reset_queues(&after);
                break;
            }
        }
        gt_emit_rep(emulated, "c13-peeks", r, out[15] ? "TIMEOUT" : "PASS", out[11], out, 17);
    }
}

/* C4: a SIGNAL (behaviour CONTINUE) halfway through the long list.  out0
   signal count, out1 signal id, out2 signal us, out3 fin us, out4 fin count,
   out5 timed out. */
static void gt_cell_signal(int emulated) {
    probe_step(emulated, "ge-timing", "c4");
    const uint32_t bytes = gt_build_long(1);
    for (uint32_t r = 0; r < GT_C_REPS; r++) {
        uint32_t out[7];
        memset(out, 0, sizeof(out));
        out[6] = bytes;
        s_gt_sig_count = 0;
        uint32_t fin = 0, idle = 0, enq = 0;
        int err = 0;
        void *lists[1] = {s_gt_list};
        const int rc = gt_run(lists, 1, &fin, NULL, &idle, &enq, &err);
        const uint32_t t0 = s_gt_last_t0;
        out[0] = s_gt_sig_count;
        out[1] = s_gt_sig_id;
        out[2] = s_gt_sig_count ? s_gt_sig_time - t0 : 0u;
        out[3] = fin;
        out[4] = s_gt_fin_count;
        out[5] = rc > 0;
        gt_emit_rep(emulated, "c4-signal", r, rc < 0 ? "FAIL" : rc ? "TIMEOUT" : "PASS",
                    out[2], out, 7);
    }
}

/* C5: an empty list enqueued while the long list runs.  out0 first enqueue
   us, out1 second enqueue return us, out2/3 first/second finish us, out4 idle
   us, out5 finish count, out6 timed out. */
static void gt_cell_busy_enqueue(int emulated) {
    probe_step(emulated, "ge-timing", "c5");
    (void)gt_build_long(0);
    gt_begin(s_gt_list_b);
    (void)gt_end();
    for (uint32_t r = 0; r < GT_C_REPS; r++) {
        uint32_t out[7];
        memset(out, 0, sizeof(out));
        s_gt_fin_count = 0;
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const int qa = sceGeListEnQueue(s_gt_list, NULL, s_gt_cbid, NULL);
        out[0] = sceKernelGetSystemTimeLow() - t0;
        const int qb = qa >= 0 ? sceGeListEnQueue(s_gt_list_b, NULL, s_gt_cbid, NULL) : qa;
        out[1] = sceKernelGetSystemTimeLow() - t0;
        if (qa < 0 || qb < 0) {
            GeIdleWait after;
            (void)ge_reset_queues(&after);
            out[6] = (uint32_t)(qa < 0 ? qa : qb);
            gt_emit_rep(emulated, "c5-busyenq", r, "FAIL", out[6], out, 7);
            continue;
        }
        uint32_t t_idle = 0;
        for (;;) {
            const int d = sceGeDrawSync(1);
            const uint32_t t = sceKernelGetSystemTimeLow() - t0;
            if (!t_idle && d == PSP_GE_LIST_DONE) t_idle = t;
            if (t_idle && s_gt_fin_count >= 2u) break;
            if (t >= GT_DEADLINE_US) {
                GeIdleWait after;
                out[6] = 1;
                (void)ge_reset_queues(&after);
                break;
            }
        }
        out[2] = s_gt_fin_count > 0u ? s_gt_fin_time[0] - t0 : 0u;
        out[3] = s_gt_fin_count > 1u ? s_gt_fin_time[1] - t0 : 0u;
        out[4] = t_idle;
        out[5] = s_gt_fin_count;
        gt_emit_rep(emulated, "c5-busyenq", r, out[6] ? "TIMEOUT" : "PASS", out[1], out, 7);
    }
}

static volatile uint32_t s_gt_sync_mode;
static volatile uint32_t s_gt_sync_out[5];

/* Helper for C2/C6: enqueue the long list, then block in sceGeDrawSync(0)
   (mode 0) or sceGeListSync(qid, 0) (mode 1).  The main thread bounds the
   whole helper with sceKernelWaitThreadEnd. */
static int gt_sync_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    s_gt_fin_count = 0;
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const int qid = sceGeListEnQueue(s_gt_list, NULL, s_gt_cbid, NULL);
    s_gt_sync_out[1] = sceKernelGetSystemTimeLow() - t0;
    int rc = qid;
    if (qid >= 0) rc = s_gt_sync_mode ? sceGeListSync(qid, 0) : sceGeDrawSync(0);
    s_gt_sync_out[2] = sceKernelGetSystemTimeLow() - t0;
    s_gt_sync_out[3] = s_gt_fin_count;
    s_gt_sync_out[4] = s_gt_fin_count ? s_gt_fin_time[0] - t0 : 0u;
    s_gt_sync_out[0] = (uint32_t)rc;
    return 0;
}

/* C2 (DrawSync(0)) and C6 (ListSync(qid, 0)) on the long list.  out0 sync
   result, out1 enqueue us, out2 sync return us, out3 finish count at return,
   out4 fin us, out5 sceKernelWaitThreadEnd result, out6 fin count after idle. */
static void gt_cell_blocking_sync(int emulated) {
    probe_step(emulated, "ge-timing", "c2-c6");
    (void)gt_build_long(0);
    for (uint32_t mode = 0; mode < 2u; mode++) {
        for (uint32_t r = 0; r < GT_C_REPS; r++) {
            uint32_t out[7];
            memset(out, 0, sizeof(out));
            memset((void *)s_gt_sync_out, 0, sizeof(s_gt_sync_out));
            s_gt_sync_mode = mode;
            sceKernelDcacheWritebackAll();
            const SceUID th = sceKernelCreateThread("gt-sync", gt_sync_thread, 0x18, 0x4000,
                                                    PSP_THREAD_ATTR_USER, NULL);
            int wrc = th;
            if (th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0) {
                SceUInt timeout = 500000u;
                wrc = sceKernelWaitThreadEnd(th, &timeout);
            }
            if (wrc < 0) {
                GeIdleWait after;
                (void)ge_reset_queues(&after);
            }
            if (th >= 0) {
                if (wrc >= 0) (void)sceKernelDeleteThread(th);
                else (void)sceKernelTerminateDeleteThread(th);
            }
            const GeIdleWait idle = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);
            for (int i = 0; i < 5; i++) out[i] = s_gt_sync_out[i];
            out[5] = (uint32_t)wrc;
            out[6] = s_gt_fin_count;
            gt_emit_rep(emulated, mode ? "c6-listsync" : "c2-drawsync", r,
                        wrc < 0 || !idle.idle ? "TIMEOUT" : "PASS", out[2], out, 7);
        }
    }
}


/* Sprites or two-triangle quads of w x h px tiling a 256 x 256 region from the
   origin; returns the vertex count. */
static uint32_t gt_prepare_tiles(GtFlatV *v, int w, int h, int quads) {
    uint32_t n = 0;
    for (int y = 0; y < 256; y += h) {
        for (int x = 0; x < 256; x += w) {
            const int16_t x0 = (int16_t)x, y0 = (int16_t)y, x1 = (int16_t)(x + w), y1 = (int16_t)(y + h);
            if (quads) {
                v[n++] = (GtFlatV){GT_COLOR_A, x0, y0, 0, 0};
                v[n++] = (GtFlatV){GT_COLOR_A, x1, y0, 0, 0};
                v[n++] = (GtFlatV){GT_COLOR_A, x0, y1, 0, 0};
                v[n++] = (GtFlatV){GT_COLOR_A, x1, y0, 0, 0};
                v[n++] = (GtFlatV){GT_COLOR_A, x1, y1, 0, 0};
                v[n++] = (GtFlatV){GT_COLOR_A, x0, y1, 0, 0};
            } else {
                v[n++] = (GtFlatV){GT_COLOR_A, x0, y0, 0, 0};
                v[n++] = (GtFlatV){GT_COLOR_A, x1, y1, 0, 0};
            }
        }
    }
    return n;
}

/* S1-S4, the second round: how fill cost depends on primitive shape, how
   vertex and raster work combine in mid-sized triangles, where the texture
   cache stops holding a texture, and whether a vertex-bound draw and a
   fill-bound draw in one list add.  The size word is the primitive count
   (S4: 0); units are pixels (S4: transformed vertices + cleared pixels). */
static void gt_cells_round2(int emulated) {
    probe_step(emulated, "ge-timing", "s1-s4");
    static const int dims[6] = {8, 16, 32, 64, 128, 256};
    const uint32_t vt = GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D;
    for (int wi = 0; wi < 6; wi++) {
        for (int hi = 0; hi < 6; hi++) {
            const uint32_t n = gt_prepare_tiles((GtFlatV *)s_gt_vtx, dims[wi], dims[hi], 0);
            char name[48];
            snprintf(name, sizeof(name), "s1-sprite-%dx%d", dims[wi], dims[hi]);
            gt_setup_base(GU_PSM_8888);
            gt_cell_draw(emulated, name, n / 2u, 65536u, GU_SPRITES, vt, n, NULL);
        }
    }
    for (int d = 0; d < 6; d++) {
        const uint32_t n = gt_prepare_tiles((GtFlatV *)s_gt_vtx, dims[d], dims[d], 1);
        char name[48];
        snprintf(name, sizeof(name), "s2-triquad-%d", dims[d]);
        gt_setup_base(GU_PSM_8888);
        gt_cell_draw(emulated, name, n / 3u, 65536u, GU_TRIANGLES, vt, n, NULL);
    }
    static const struct { const char *name; int psm; int swizzle; } formats[5] = {
        {"8888", GU_PSM_8888, 0}, {"8888s", GU_PSM_8888, 1}, {"5650", GU_PSM_5650, 0},
        {"t8", GU_PSM_T8, 0}, {"dxt1", GU_PSM_DXT1, 0},
    };
    static const int sizes[2] = {128, 256};
    for (int f = 0; f < 5; f++) {
        for (int z = 0; z < 2; z++) {
            for (int c = 0; c < 3; c++) {
                const uint32_t count = s_gt_counts[c];
                gt_begin(s_gt_setup);
                gt_base_state(GU_PSM_8888);
                sceGuEnable(GU_TEXTURE_2D);
                sceGuTexMode(formats[f].psm, 0, 0, formats[f].swizzle);
                sceGuTexImage(0, sizes[z], sizes[z], sizes[z], s_gt_tex);
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
                sceGuTexFilter(GU_NEAREST, GU_NEAREST);
                sceGuTexWrap(GU_REPEAT, GU_REPEAT);
                if (formats[f].psm == GU_PSM_T8) {
                    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
                    sceGuClutLoad(32, s_gt_clut);
                }
                sceGuTexFlush();
                (void)gt_end();
                gt_begin(s_gt_list);
                for (uint32_t k = 0; k < count; k++)
                    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                                   16, NULL, s_gt_tex_strips);
                char name[40];
                snprintf(name, sizeof(name), "s3-tex-%s-%d", formats[f].name, sizes[z]);
                gt_measure_built(emulated, name, count, count * GT_W * GT_H, gt_end(), GT_REPS);
            }
        }
    }
    /* S4: 6144 tiny transformed vertices, then four 8888 colour clears, in one list. */
    const uint32_t xf = GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    (void)gt_pack_tiny(xf, 6144u);
    gt_setup_base(GU_PSM_8888);
    gt_begin(s_gt_list);
    sceGuDrawArray(GU_TRIANGLES, (int)xf, 6144, NULL, s_gt_vtx);
    for (int k = 0; k < 4; k++) {
        sceGuSendCommandi(211, (GU_COLOR_BUFFER_BIT << 8) | 1);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 16, NULL, s_gt_strips);
        sceGuSendCommandi(211, 0);
    }
    gt_measure_built(emulated, "s4-mix-vertex-fill", 0u, 6144u + 4u * GT_W * GT_H, gt_end(), GT_REPS);
}

static void gt_pass(int emulated, int pll, int cpu, int bus) {
    const int rc = scePowerSetClockFrequency(pll, cpu, bus);
    s_gt_cpu_mhz = (uint32_t)scePowerGetCpuClockFrequencyInt();
    s_gt_bus_mhz = (uint32_t)scePowerGetBusClockFrequencyInt();
    snprintf(s_gt_clk, sizeof(s_gt_clk), "b%u", (unsigned int)bus);
    if (rc < 0 || s_gt_bus_mhz != (uint32_t)bus) {
        const uint32_t out[3] = {(uint32_t)rc, s_gt_cpu_mhz, s_gt_bus_mhz};
        char case_id[32];
        snprintf(case_id, sizeof(case_id), "gt-%s-clock", s_gt_clk);
        gt_record(emulated, case_id, "SKIP", (uint32_t)rc, out, 3);
        return;
    }
    gt_cells_commands(emulated);
    gt_cells_clear(emulated);
    gt_cells_through(emulated);
    gt_cells_transform(emulated);
    gt_cells_texture(emulated);
    gt_cells_fill(emulated);
    gt_cells_upload(emulated);
    gt_cell_two_lists(emulated);
    gt_cells_round2(emulated);
    gt_setup_base(GU_PSM_8888);
    {
        GtStats setup;
        gt_apply_setup(&setup);
    }
    gt_cell_peeks(emulated, gt_build_long(0));
    gt_cell_signal(emulated);
    gt_cell_busy_enqueue(emulated);
    gt_cell_blocking_sync(emulated);
}

static void run_ge_timing(int emulated) {
    const int boot_cpu = scePowerGetCpuClockFrequencyInt();
    const int boot_bus = scePowerGetBusClockFrequencyInt();
    /* Static buffers were zeroed through the cache; write those lines back and
       drop them so no later eviction lands on a list libpspgu writes. */
    sceKernelDcacheWritebackInvalidateAll();
    const int init_rc = sceGuInit();
    PspGeCallbackData cb;
    memset(&cb, 0, sizeof(cb));
    cb.signal_func = gt_signal_cb;
    cb.finish_func = gt_finish_cb;
    s_gt_cbid = sceGeSetCallback(&cb);
    {
        const uint32_t out[4] = {(uint32_t)init_rc, (uint32_t)s_gt_cbid, (uint32_t)boot_cpu,
                                 (uint32_t)boot_bus};
        gt_record(emulated, "gt-setup", init_rc >= 0 && s_gt_cbid >= 0 ? "PASS" : "FAIL", 0u,
                  out, 4);
    }
    uint32_t passes = 0;
    if (init_rc >= 0 && s_gt_cbid >= 0) {
        for (uint32_t i = 0; i < 512u * 512u; i++) s_gt_tex[i] = i * 2654435761u;
        for (uint32_t i = 0; i < 256u; i++) s_gt_clut[i] = 0xff000000u | (i * 0x010203u);
        gt_prepare_strips();
        /* VRAM-to-VRAM transfer source: fill it once through the uncached mirror. */
        volatile uint32_t *src = (volatile uint32_t *)((uintptr_t)GT_VRAM_SRC | 0x40000000u);
        for (uint32_t i = 0; i < 512u * 272u; i++) src[i] = i;
        sceKernelDcacheWritebackAll();
        gt_pass(emulated, 222, 222, 111);
        passes++;
        gt_pass(emulated, 333, 333, 166);
        passes++;
    }
    /* Reached on every path once sceGuInit has run, whether or not the
       callback registration or the passes succeeded. */
    const GeGuTeardown gu_down = ge_gu_teardown(init_rc, s_gt_cbid);
    s_gt_cbid = -1;
    {
        uint32_t out[GE_GU_TEARDOWN_OUTS];
        ge_gu_teardown_outs(&gu_down, out);
        gt_record(emulated, "gt-gu-teardown", gu_down.idle ? "PASS" : "TIMEOUT",
                  (uint32_t)gu_down.draw_state, out, GE_GU_TEARDOWN_OUTS);
    }
    const int restore_rc = scePowerSetClockFrequency(boot_cpu, boot_cpu, boot_bus);
    const uint32_t done_out[4] = {passes, (uint32_t)restore_rc,
                                  (uint32_t)scePowerGetCpuClockFrequencyInt(),
                                  (uint32_t)scePowerGetBusClockFrequencyInt()};
    gt_record(emulated, "gt-done", restore_rc >= 0 ? "PASS" : "FAIL", passes, done_out, 4);
    /* Stream completion: out0 is the number of records above, which the runner
       checks against the ordered contract in tools/psp_oracle/parse_golden.py. */
    const uint32_t records = s_gt_records;
    emit_record_extended(emulated, "PSP-GE-TIMING-001", "ge-timing-done", "PASS", 0u,
                         &records, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR
_Static_assert(DEFERRED_MAX_RECORDS == VRAM_MIRROR_SEMANTIC_RECORDS + 1u,
               "VRAM mirror stream must fit its deferred record buffer");
_Static_assert(VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT == 4134u,
               "VRAM mirror sample set must include every requested position");
_Static_assert(VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT == 517u,
               "VRAM mirror sample chunk count must match the host protocol");
_Static_assert(VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT == 16u,
               "VRAM mirror depth rows must be split into bounded chunks");
_Static_assert(VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT == 8u,
               "VRAM mirror color rows must be split into bounded chunks");
_Static_assert(VRAM_MIRROR_SEMANTIC_RECORDS == 725u,
               "VRAM mirror record count must match the host protocol");
_Static_assert(VRAM_MIRROR_WRITE_OFFSET_2 < 0x00100000u,
               "VRAM mirror marker alias bits must not overlap sample offsets");
_Static_assert(VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS * 8u * 2u <= DEFERRED_MAX_OUT,
               "VRAM mirror alias samples must fit deferred records");
_Static_assert(VRAM_MIRROR_DEPTH_CHUNK_PIXELS *
                   VRAM_MIRROR_DEPTH_ALIAS_COUNT * 2u == DEFERRED_MAX_OUT,
               "VRAM mirror depth rows must fill deferred records");
_Static_assert(VRAM_MIRROR_COLOR_CHUNK_PIXELS * 2u == DEFERRED_MAX_OUT,
               "VRAM mirror color rows must fill deferred records");

static const uint32_t s_vram_mirror_aliases[8] = {
    0x04000000u, 0x04200000u, 0x04400000u, 0x04600000u,
    0x44000000u, 0x44200000u, 0x44400000u, 0x44600000u,
};
static const uint32_t s_vram_mirror_write_aliases[6] = {
    0x04200000u, 0x04400000u, 0x04600000u,
    0x44200000u, 0x44400000u, 0x44600000u,
};

static const uint32_t s_vram_mirror_depth_aliases[VRAM_MIRROR_DEPTH_ALIAS_COUNT] = {
    0x44000000u, 0x44200000u,
};
static const uint32_t s_vram_mirror_color_alias = 0x44000000u;

static const uint32_t s_vram_mirror_depth_rows[VRAM_MIRROR_DEPTH_ROW_COUNT] = {
    VRAM_MIRROR_DEPTH_ROW_Y0, VRAM_MIRROR_DEPTH_ROW_Y1,
    VRAM_MIRROR_DEPTH_ROW_Y2, VRAM_MIRROR_DEPTH_ROW_Y3,
    VRAM_MIRROR_DEPTH_ROW_Y4, VRAM_MIRROR_DEPTH_ROW_Y5,
    VRAM_MIRROR_DEPTH_ROW_Y6, VRAM_MIRROR_DEPTH_ROW_Y7,
    VRAM_MIRROR_DEPTH_ROW_Y8, VRAM_MIRROR_DEPTH_ROW_Y9,
};
static const uint32_t s_vram_mirror_color_rows[VRAM_MIRROR_COLOR_ROW_COUNT] = {
    VRAM_MIRROR_COLOR_ROW_Y0, VRAM_MIRROR_COLOR_ROW_Y1,
    VRAM_MIRROR_COLOR_ROW_Y2,
};

typedef struct {
    uint16_t x0;
    uint16_t y0;
    uint16_t z0;
    uint16_t x1;
    uint16_t y1;
    uint16_t z1;
    uint32_t color;
} VramMirrorSprite;

static const VramMirrorSprite s_vram_mirror_sprites[3] = {
    {0u, 0u, 0x1111u, 32u, 16u, 0x1111u, 0xffff0000u},
    {64u, 8u, 0x2222u, 96u, 12u, 0x2222u, 0xff00ff00u},
    {200u, 100u, 0x3333u, 208u, 101u, 0x3333u, 0xff0000ffu},
};

struct vram_mirror_sprite_vertex {
    uint32_t color;
    int16_t x;
    int16_t y;
    int16_t z;
};
_Static_assert(sizeof(struct vram_mirror_sprite_vertex) == 12u,
               "GU 16-bit through-mode sprite vertices have a 12-byte stride");

typedef struct {
    int completed;
    int ge_idle;
} VramMirrorGeOutcome;

static uint32_t s_vram_mirror_list[4096] __attribute__((aligned(64)));
static struct vram_mirror_sprite_vertex s_vram_mirror_vertices[6]
    __attribute__((aligned(16)));

static void vram_mirror_cache_range(uint32_t offset, uint32_t bytes,
                                   int writeback_invalidate) {
    for (uint32_t alias = 0; alias < VRAM_MIRROR_CACHED_ALIAS_COUNT; alias++) {
        const uint32_t address = s_vram_mirror_aliases[alias] + offset;
        if (writeback_invalidate) {
            sceKernelDcacheWritebackInvalidateRange(
                (void *)(uintptr_t)address, bytes);
        } else {
            sceKernelDcacheInvalidateRange((void *)(uintptr_t)address, bytes);
        }
    }
}

static uint32_t s_vram_mirror_sample_offsets[VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT];

static int vram_mirror_build_sample_offsets(void) {
    static const uint32_t pair_bases[VRAM_MIRROR_CPU_PAIR_REGION_COUNT] = {
        0x00000000u, 0x00044000u, 0x00100000u, 0x001ff800u,
    };
    uint32_t sample = 0;

    for (uint32_t row = 0; row < VRAM_MIRROR_CPU_DENSE_ROWS; row++) {
        for (uint32_t word = 0; word < VRAM_MIRROR_ROW_WORDS; word++) {
            s_vram_mirror_sample_offsets[sample++] =
                VRAM_MIRROR_CPU_DENSE_OFFSET + row * VRAM_MIRROR_ROW_BYTES +
                word * 4u;
        }
    }
    for (uint32_t region = 0; region < VRAM_MIRROR_CPU_PAIR_REGION_COUNT; region++) {
        for (uint32_t row = 0; row < VRAM_MIRROR_CPU_PAIR_ROWS; row++) {
            for (uint32_t word = 0; word < VRAM_MIRROR_ROW_WORDS; word++) {
                s_vram_mirror_sample_offsets[sample++] = pair_bases[region] +
                    row * VRAM_MIRROR_ROW_BYTES + word * 4u;
            }
        }
    }
    for (uint32_t bit = VRAM_MIRROR_CPU_SWEEP_FIRST_BIT;
         bit <= VRAM_MIRROR_CPU_SWEEP_LAST_BIT; bit++) {
        const uint32_t power = 1u << bit;
        s_vram_mirror_sample_offsets[sample++] =
            VRAM_MIRROR_CPU_DENSE_OFFSET ^ power;
        s_vram_mirror_sample_offsets[sample++] = power;
    }
    return sample == VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT;
}

static void vram_mirror_skip_stream(int emulated, uint32_t reason) {
    char case_id[64];
    const uint32_t done = VRAM_MIRROR_SEMANTIC_RECORDS;
    defer_record("vram-mirror-info", "SKIP", reason, NULL, 0);
    for (uint32_t chunk = 0; chunk < VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT; chunk++) {
        snprintf(case_id, sizeof(case_id), "vram-cpu-pattern-%03u",
                 (unsigned int)chunk);
        defer_record(case_id, "SKIP", reason, NULL, 0);
    }
    for (uint32_t alias = 0; alias < VRAM_MIRROR_WRITE_ALIAS_COUNT; alias++) {
        for (uint32_t sample = 0; sample < VRAM_MIRROR_WRITE_SAMPLE_COUNT; sample++) {
            snprintf(case_id, sizeof(case_id), "vram-cpu-write-mirror-%02u-%02u",
                     (unsigned int)alias, (unsigned int)sample);
            defer_record(case_id, "SKIP", reason, NULL, 0);
        }
    }
    defer_record("vram-ge-clear-sync", "SKIP", reason, NULL, 0);
    defer_record("vram-ge-sprite-a-sync", "SKIP", reason, NULL, 0);
    defer_record("vram-ge-sprite-b-sync", "SKIP", reason, NULL, 0);
    defer_record("vram-ge-sprite-c-sync", "SKIP", reason, NULL, 0);
    for (uint32_t row = 0; row < VRAM_MIRROR_DEPTH_ROW_COUNT; row++) {
        for (uint32_t chunk = 0; chunk < VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT; chunk++) {
            snprintf(case_id, sizeof(case_id), "vram-ge-depth-row%u-%02u",
                     (unsigned int)row, (unsigned int)chunk);
            defer_record(case_id, "SKIP", reason, NULL, 0);
        }
    }
    for (uint32_t row = 0; row < VRAM_MIRROR_COLOR_ROW_COUNT; row++) {
        for (uint32_t chunk = 0; chunk < VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT; chunk++) {
            snprintf(case_id, sizeof(case_id), "vram-ge-color-row%u-%02u",
                     (unsigned int)row, (unsigned int)chunk);
            defer_record(case_id, "SKIP", reason, NULL, 0);
        }
    }
    defer_record("vram-restore", "SKIP", reason, NULL, 0);
    defer_record("vram-mirror-done", "SKIP", reason, &done, 1);
    flush_deferred(emulated, "PSP-VRAM-MIRROR-001");
}

static void vram_mirror_read_words(const uint32_t *offsets, uint32_t word_count,
                                   uint32_t out[128]) {
    uint32_t output = 0;
    for (uint32_t word = 0; word < word_count; word++) {
        for (uint32_t alias = 0; alias < 8u; alias++) {
            const uint32_t address = s_vram_mirror_aliases[alias] + offsets[word];
            out[output++] = address;
            out[output++] = *(volatile uint32_t *)(uintptr_t)address;
        }
    }
}

static void vram_mirror_defer_pattern_samples(void) {
    uint32_t offsets[VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS];
    uint32_t out[DEFERRED_MAX_OUT];
    char case_id[64];

    for (uint32_t chunk = 0; chunk < VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT; chunk++) {
        const uint32_t first = chunk * VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS;
        uint32_t word_count = VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT - first;
        if (word_count > VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS) {
            word_count = VRAM_MIRROR_CPU_SAMPLE_CHUNK_WORDS;
        }
        for (uint32_t word = 0; word < word_count; word++) {
            offsets[word] = s_vram_mirror_sample_offsets[first + word];
        }
        vram_mirror_read_words(offsets, word_count, out);
        snprintf(case_id, sizeof(case_id), "vram-cpu-pattern-%03u",
                 (unsigned int)chunk);
        defer_record(case_id, "PASS", 0u, out, word_count * 8u * 2u);
    }
}

static void vram_mirror_probe_writes(void) {
    static const uint32_t offsets[VRAM_MIRROR_WRITE_SAMPLE_COUNT] = {
        VRAM_MIRROR_WRITE_OFFSET_0,
        VRAM_MIRROR_WRITE_OFFSET_1,
        VRAM_MIRROR_WRITE_OFFSET_2,
    };
    char case_id[64];

    for (uint32_t alias = 0; alias < VRAM_MIRROR_WRITE_ALIAS_COUNT; alias++) {
        const uint32_t write_base = s_vram_mirror_write_aliases[alias];
        const int uncached = (write_base & 0x40000000u) != 0u;
        for (uint32_t sample = 0; sample < VRAM_MIRROR_WRITE_SAMPLE_COUNT; sample++) {
            const uint32_t offset = offsets[sample];
            const uint32_t write_address = write_base + offset;
            const uint32_t marker = VRAM_MIRROR_MARKER_PREFIX |
                (alias << 20) | offset;
            uint32_t first_found = 0xffffffffu;
            uint32_t found_count = 0u;

            vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 0);
            *(volatile uint32_t *)(uintptr_t)write_address = marker;
            if (!uncached) {
                sceKernelDcacheWritebackInvalidateRange(
                    (void *)(uintptr_t)write_address, sizeof(marker));
            }
            vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 0);

            volatile uint32_t *const base_words =
                (volatile uint32_t *)(uintptr_t)0x44000000u;
            for (uint32_t scan_offset = 0; scan_offset < VRAM_MIRROR_SIZE;
                 scan_offset += sizeof(uint32_t)) {
                if (base_words[scan_offset / sizeof(uint32_t)] == marker) {
                    if (found_count == 0u) first_found = scan_offset;
                    found_count++;
                    base_words[scan_offset / sizeof(uint32_t)] =
                        VRAM_MIRROR_PATTERN_PREFIX | scan_offset;
                }
            }
            vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 0);

            const uint32_t out[4] = {
                write_address, marker, first_found, found_count,
            };
            snprintf(case_id, sizeof(case_id), "vram-cpu-write-mirror-%02u-%02u",
                     (unsigned int)alias, (unsigned int)sample);
            defer_record(case_id, "PASS", 0u, out, 4u);
        }
    }
}

enum {
    VRAM_MIRROR_GE_CLEAR = 0,
    VRAM_MIRROR_GE_SPRITE_A = 1,
    VRAM_MIRROR_GE_SPRITE_B = 2,
    VRAM_MIRROR_GE_SPRITE_C = 3,
};

static VramMirrorGeOutcome vram_mirror_ge_step(uint32_t color_offset,
                                               uint32_t depth_offset,
                                               int gu_init_rc,
                                               uint32_t operation,
                                               int start_allowed) {
    static const char *const sync_cases[VRAM_MIRROR_GE_SYNC_COUNT] = {
        "vram-ge-clear-sync",
        "vram-ge-sprite-a-sync",
        "vram-ge-sprite-b-sync",
        "vram-ge-sprite-c-sync",
    };
    const int start_rc = !start_allowed ? -1 : gu_init_rc >= 0
        ? sceGuStart(GU_DIRECT, s_vram_mirror_list) : gu_init_rc;
    int finish_rc = -1;
    int reset_rc = 0;
    int reset_idle = 1;
    GeIdleWait wait = {-1, -1, 0u, 0};
    uint32_t out[VRAM_MIRROR_SYNC_OUT_COUNT] = {0};
    const VramMirrorSprite *sprite = operation == VRAM_MIRROR_GE_CLEAR
        ? NULL : &s_vram_mirror_sprites[operation - 1u];
    const uint32_t vertex_type =
        GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D;

    if (start_rc >= 0) {
        sceGuDrawBuffer(GU_PSM_5650, (void *)(uintptr_t)color_offset,
                        VRAM_MIRROR_DEPTH_STRIDE);
        sceGuDepthBuffer((void *)(uintptr_t)depth_offset,
                         VRAM_MIRROR_DEPTH_STRIDE);
        sceGuOffset(2048 - 256, 2048 - 136);
        sceGuViewport(2048, 2048, VRAM_MIRROR_DEPTH_STRIDE, 272);
        sceGuDepthRange(VRAM_MIRROR_DEPTH_RANGE_NEAR,
                        VRAM_MIRROR_DEPTH_RANGE_FAR);
        sceGuScissor(0, 0, VRAM_MIRROR_DEPTH_STRIDE, 272);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuDisable(GU_CULL_FACE);
        sceGuDisable(GU_BLEND);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_STENCIL_TEST);
        sceGuDisable(GU_COLOR_TEST);
        sceGuDisable(GU_COLOR_LOGIC_OP);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(GU_ALWAYS);
        sceGuDepthMask(GU_FALSE);

        if (operation == VRAM_MIRROR_GE_CLEAR) {
            sceGuClearDepth(VRAM_MIRROR_CLEAR_DEPTH);
            sceGuClear(GU_DEPTH_BUFFER_BIT);
        } else {
            const uint32_t vertex_index =
                (operation - VRAM_MIRROR_GE_SPRITE_A) * 2u;
            s_vram_mirror_vertices[vertex_index] =
                (struct vram_mirror_sprite_vertex){
                    sprite->color, (int16_t)sprite->x0, (int16_t)sprite->y0,
                    (int16_t)sprite->z0,
                };
            s_vram_mirror_vertices[vertex_index + 1u] =
                (struct vram_mirror_sprite_vertex){
                    sprite->color, (int16_t)sprite->x1, (int16_t)sprite->y1,
                    (int16_t)sprite->z1,
                };
            sceKernelDcacheWritebackRange(s_vram_mirror_vertices,
                                          sizeof(s_vram_mirror_vertices));
            sceGuDrawArray(GU_SPRITES, vertex_type, 2, NULL,
                           &s_vram_mirror_vertices[vertex_index]);
        }
        finish_rc = sceGuFinish();
    }

    wait = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);
    if (!wait.idle) {
        GeIdleWait after_reset;
        reset_rc = ge_reset_queues(&after_reset);
        reset_idle = after_reset.idle;
    }
    vram_mirror_cache_range(depth_offset, VRAM_MIRROR_FRAME_BYTES, 0);
    out[0] = (uint32_t)start_rc;
    out[1] = (uint32_t)finish_rc;
    out[2] = (uint32_t)wait.draw_state;
    out[3] = wait.elapsed_us;
    out[4] = wait.idle ? 1u : 0u;
    out[5] = (uint32_t)reset_rc;
    out[6] = reset_idle ? 1u : 0u;
    out[7] = VRAM_MIRROR_CACHED_ALIAS_COUNT;
    out[8] = depth_offset;
    out[9] = VRAM_MIRROR_DEPTH_STRIDE;
    out[10] = VRAM_MIRROR_DEPTH_RANGE_NEAR;
    out[11] = VRAM_MIRROR_DEPTH_RANGE_FAR;
    out[12] = GU_ALWAYS;
    out[13] = GU_FALSE;
    out[14] = VRAM_MIRROR_DEPTH_TEST_ENABLED;
    out[15] = VRAM_MIRROR_CLEAR_DEPTH;
    out[16] = operation;
    if (sprite != NULL) {
        out[17] = sprite->x0;
        out[18] = sprite->y0;
        out[19] = sprite->z0;
        out[20] = sprite->x1;
        out[21] = sprite->y1;
        out[22] = sprite->z1;
        out[23] = sprite->color;
        out[24] = 2u;
        out[25] = GU_SPRITES;
        out[26] = vertex_type;
    }
    const int completed = start_rc >= 0 && finish_rc >= 0 && wait.idle && reset_idle;
    const int ge_idle = wait.idle ? 1 : reset_idle;
    defer_record(sync_cases[operation],
                 !wait.idle ? "TIMEOUT" : completed ? "PASS" : "FAIL",
                 (uint32_t)(finish_rc < 0 ? finish_rc : start_rc), out,
                 VRAM_MIRROR_SYNC_OUT_COUNT);
    return (VramMirrorGeOutcome){completed, ge_idle};
}

static void vram_mirror_defer_depth_rows(uint32_t depth_offset, int ge_idle,
                                         int ge_sequence_complete) {
    uint32_t out[DEFERRED_MAX_OUT];
    char case_id[64];

    for (uint32_t row = 0; row < VRAM_MIRROR_DEPTH_ROW_COUNT; row++) {
        for (uint32_t chunk = 0; chunk < VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT; chunk++) {
            snprintf(case_id, sizeof(case_id), "vram-ge-depth-row%u-%02u",
                     (unsigned int)row, (unsigned int)chunk);
            if (!ge_idle) {
                defer_record(case_id, "SKIP", 7u, NULL, 0);
                continue;
            }
            uint32_t output = 0;
            for (uint32_t pixel = 0; pixel < VRAM_MIRROR_DEPTH_CHUNK_PIXELS; pixel++) {
                const uint32_t x = chunk * VRAM_MIRROR_DEPTH_CHUNK_PIXELS + pixel;
                const uint32_t offset =
                    depth_offset +
                    (s_vram_mirror_depth_rows[row] * VRAM_MIRROR_DEPTH_STRIDE + x) * 2u;
                for (uint32_t alias = 0;
                     alias < VRAM_MIRROR_DEPTH_ALIAS_COUNT; alias++) {
                    const uint32_t address =
                        s_vram_mirror_depth_aliases[alias] + offset;
                    out[output++] = address;
                    out[output++] = *(volatile uint16_t *)(uintptr_t)address;
                }
            }
            defer_record(case_id, ge_sequence_complete ? "PASS" : "FAIL",
                         ge_sequence_complete ? 0u : 1u, out, output);
        }
    }
}

static void vram_mirror_defer_color_rows(uint32_t color_offset, int ge_idle,
                                         int ge_sequence_complete) {
    uint32_t out[DEFERRED_MAX_OUT];
    char case_id[64];

    for (uint32_t row = 0; row < VRAM_MIRROR_COLOR_ROW_COUNT; row++) {
        for (uint32_t chunk = 0; chunk < VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT; chunk++) {
            snprintf(case_id, sizeof(case_id), "vram-ge-color-row%u-%02u",
                     (unsigned int)row, (unsigned int)chunk);
            if (!ge_idle) {
                defer_record(case_id, "SKIP", 7u, NULL, 0);
                continue;
            }
            uint32_t output = 0;
            for (uint32_t pixel = 0; pixel < VRAM_MIRROR_COLOR_CHUNK_PIXELS; pixel++) {
                const uint32_t x = chunk * VRAM_MIRROR_COLOR_CHUNK_PIXELS + pixel;
                const uint32_t offset =
                    color_offset +
                    (s_vram_mirror_color_rows[row] * VRAM_MIRROR_DEPTH_STRIDE + x) * 2u;
                const uint32_t address = s_vram_mirror_color_alias + offset;
                out[output++] = address;
                out[output++] = *(volatile uint16_t *)(uintptr_t)address;
            }
            defer_record(case_id, ge_sequence_complete ? "PASS" : "FAIL",
                         ge_sequence_complete ? 0u : 1u, out, output);
        }
    }
}

static void run_vram_mirror(int emulated) {
    void *display_address = NULL;
    int display_stride = 0;
    int display_format = 0;
    const uint32_t raw_vram_base = (uint32_t)(uintptr_t)sceGeEdramGetAddr();
    const uint32_t vram_base = raw_vram_base & ~0x40000000u;
    int skip_reason = 0;
    int display_rc = -1;
    uint32_t display_offset = 0;
    uint32_t display_bytes = 0;
    uint32_t color_offset = 0;
    uint32_t depth_offset = 0;
    uint8_t *backup = NULL;
    uint32_t restore_mismatches = 0;

    probe_step(emulated, "vram-mirror", "setup");
    if (vram_base != 0x04000000u) {
        skip_reason = 1u;
    } else {
        display_rc = sceDisplayGetFrameBuf(&display_address, &display_stride,
                                           &display_format,
                                           PSP_DISPLAY_SETBUF_IMMEDIATE);
        if (display_rc < 0 || display_address == NULL || display_stride <= 0 ||
            display_format < 0 || display_format > 3) {
            skip_reason = 2u;
        }
    }
    if (skip_reason == 0) {
        const uint32_t raw_display = (uint32_t)(uintptr_t)display_address;
        const uint32_t canonical_display = raw_display & ~0x40000000u;
        const uint32_t bytes_per_pixel = display_format == 3 ? 4u : 2u;
        const uint64_t display_span =
            (uint64_t)(uint32_t)display_stride * 272u * bytes_per_pixel;
        if (canonical_display < vram_base ||
            canonical_display >= vram_base + VRAM_MIRROR_SIZE ||
            display_span == 0u || display_span > VRAM_MIRROR_SIZE) {
            skip_reason = 3u;
        } else {
            display_offset = canonical_display - vram_base;
            display_bytes = (uint32_t)display_span;
            color_offset = (display_offset + display_bytes +
                            VRAM_MIRROR_ROW_BYTES - 1u) &
                ~(VRAM_MIRROR_ROW_BYTES - 1u);
            depth_offset = color_offset + VRAM_MIRROR_FRAME_BYTES +
                           (uint32_t)VRAM_MIRROR_DEPTH_BIAS;
            if (depth_offset + VRAM_MIRROR_FRAME_BYTES > VRAM_MIRROR_SIZE) {
                skip_reason = 4u;
            }
        }
    }
    if (skip_reason == 0 && !vram_mirror_build_sample_offsets()) {
        skip_reason = 5u;
    }
    if (skip_reason == 0) {
        backup = (uint8_t *)malloc(VRAM_MIRROR_SIZE);
        if (backup == NULL) skip_reason = 6u;
    }
    if (skip_reason == 0) {
        const GeIdleWait before = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);
        if (!before.idle) skip_reason = 7u;
    }
    if (skip_reason == 0) {
        vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 1);
        memcpy(backup, (const void *)(uintptr_t)0x44000000u,
               VRAM_MIRROR_SIZE);
    }
    if (skip_reason != 0) {
        free(backup);
        vram_mirror_skip_stream(emulated, (uint32_t)skip_reason);
        return;
    }

    const uint32_t info[VRAM_MIRROR_INFO_OUT_COUNT] = {
        vram_base, 0x44000000u, display_offset, display_bytes,
        (uint32_t)display_stride, (uint32_t)display_format,
        color_offset, VRAM_MIRROR_FRAME_BYTES, depth_offset,
        VRAM_MIRROR_FRAME_BYTES, VRAM_MIRROR_SIZE,
        VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT,
        VRAM_MIRROR_CPU_DENSE_OFFSET, VRAM_MIRROR_CPU_DENSE_ROWS,
        VRAM_MIRROR_ROW_BYTES, VRAM_MIRROR_CPU_PAIR_REGION_COUNT,
        VRAM_MIRROR_CPU_PAIR_ROWS,
        VRAM_MIRROR_CPU_SWEEP_FIRST_BIT,
        VRAM_MIRROR_CPU_SWEEP_LAST_BIT,
        VRAM_MIRROR_WRITE_ALIAS_COUNT, VRAM_MIRROR_WRITE_SAMPLE_COUNT,
        VRAM_MIRROR_WRITE_OFFSET_0, VRAM_MIRROR_WRITE_OFFSET_1,
        VRAM_MIRROR_WRITE_OFFSET_2, VRAM_MIRROR_SIZE,
        VRAM_MIRROR_DEPTH_STRIDE, VRAM_MIRROR_DEPTH_ROW_COUNT,
        VRAM_MIRROR_DEPTH_ROW_Y0, VRAM_MIRROR_DEPTH_ROW_Y1,
        VRAM_MIRROR_DEPTH_ROW_Y2, VRAM_MIRROR_DEPTH_ROW_Y3,
        VRAM_MIRROR_DEPTH_ROW_Y4, VRAM_MIRROR_DEPTH_ROW_Y5,
        VRAM_MIRROR_DEPTH_ROW_Y6, VRAM_MIRROR_DEPTH_ROW_Y7,
        VRAM_MIRROR_DEPTH_ROW_Y8, VRAM_MIRROR_DEPTH_ROW_Y9,
        VRAM_MIRROR_DEPTH_ALIAS_COUNT, VRAM_MIRROR_DEPTH_CHUNK_PIXELS,
        VRAM_MIRROR_COLOR_ROW_COUNT, VRAM_MIRROR_COLOR_ROW_Y0,
        VRAM_MIRROR_COLOR_ROW_Y1, VRAM_MIRROR_COLOR_ROW_Y2,
        VRAM_MIRROR_COLOR_CHUNK_PIXELS,
        VRAM_MIRROR_DEPTH_TEST_ENABLED, GU_ALWAYS, GU_FALSE,
        VRAM_MIRROR_DEPTH_RANGE_NEAR, VRAM_MIRROR_DEPTH_RANGE_FAR,
        VRAM_MIRROR_CLEAR_DEPTH, VRAM_MIRROR_PATTERN_PREFIX,
        VRAM_MIRROR_MARKER_PREFIX, VRAM_MIRROR_CACHED_ALIAS_COUNT,
    };
    defer_record("vram-mirror-info", "PASS", (uint32_t)display_rc, info,
                 VRAM_MIRROR_INFO_OUT_COUNT);

    probe_step(emulated, "vram-mirror", "fill-full-position-pattern");
    volatile uint32_t *const base_words = (volatile uint32_t *)(uintptr_t)vram_base;
    for (uint32_t offset = 0; offset < VRAM_MIRROR_SIZE; offset += sizeof(uint32_t)) {
        base_words[offset / sizeof(uint32_t)] =
            VRAM_MIRROR_PATTERN_PREFIX | offset;
    }
    vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 1);
    vram_mirror_defer_pattern_samples();

    probe_step(emulated, "vram-mirror", "cpu-mirror-write-scan");
    vram_mirror_probe_writes();

    probe_step(emulated, "vram-mirror", "ge-depth-clear");
    const int gu_init_rc = sceGuInit();
    const VramMirrorGeOutcome clear = vram_mirror_ge_step(
        color_offset, depth_offset, gu_init_rc, VRAM_MIRROR_GE_CLEAR, 1);
    probe_step(emulated, "vram-mirror", "ge-through-sprite-a");
    const VramMirrorGeOutcome sprite_a = vram_mirror_ge_step(
        color_offset, depth_offset, gu_init_rc, VRAM_MIRROR_GE_SPRITE_A,
        clear.completed);
    probe_step(emulated, "vram-mirror", "ge-through-sprite-b");
    const VramMirrorGeOutcome sprite_b = vram_mirror_ge_step(
        color_offset, depth_offset, gu_init_rc, VRAM_MIRROR_GE_SPRITE_B,
        sprite_a.completed);
    probe_step(emulated, "vram-mirror", "ge-through-sprite-c");
    const VramMirrorGeOutcome sprite_c = vram_mirror_ge_step(
        color_offset, depth_offset, gu_init_rc, VRAM_MIRROR_GE_SPRITE_C,
        sprite_b.completed);
    /* GE teardown before any CPU sample or restore.  vram-restore's GE-idle
       field (out4) records whether the GE reached idle by the end of it. */
    probe_step(emulated, "vram-mirror", "gu-teardown");
    const GeGuTeardown gu_down = ge_gu_teardown(gu_init_rc, -1);
    const int ge_idle = sprite_c.ge_idle && gu_down.idle;
    const int ge_sequence_complete = clear.completed && sprite_a.completed &&
        sprite_b.completed && sprite_c.completed;

    probe_step(emulated, "vram-mirror", "ge-depth-and-color-row-samples");
    vram_mirror_cache_range(color_offset, VRAM_MIRROR_FRAME_BYTES, 0);
    vram_mirror_cache_range(depth_offset, VRAM_MIRROR_FRAME_BYTES, 0);
    vram_mirror_defer_depth_rows(depth_offset, ge_idle, ge_sequence_complete);
    vram_mirror_defer_color_rows(color_offset, ge_idle, ge_sequence_complete);

#ifdef VRAM_MIRROR_FULL_SCAN
    /* #1049 caveat: where do the depth clear and the sprite Z values land? Count the four probe values in
       every 128 KiB block of VRAM through the uncached base alias, and record the first halfword offset of each.
       Emitted directly (not deferred) because the deferred record table is sized to the semantic records. */
    {
        static const uint16_t scan_values[4] = {0x1111u, 0x2222u, 0x3333u, 0x5a5au};
        uint32_t scan_counts[4][16];
        uint32_t scan_first[4];
        memset(scan_counts, 0, sizeof(scan_counts));
        memset(scan_first, 0, sizeof(scan_first));
        const volatile uint16_t *vram16 = (const volatile uint16_t *)(uintptr_t)0x44000000u;
        for (uint32_t i = 0; i < VRAM_MIRROR_SIZE / 2u; i++) {
            const uint16_t value = vram16[i];
            for (uint32_t k = 0; k < 4u; k++) {
                if (value == scan_values[k]) {
                    scan_counts[k][(i * 2u) >> 17] += 1u;
                    if (scan_first[k] == 0u) scan_first[k] = i * 2u + 1u;
                }
            }
        }
        char scan_line[2048];
        int used = snprintf(scan_line, sizeof(scan_line),
                            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VRAM-MIRROR-001 "
                            "case_id=vram-full-scan status=PASS result=0x00000000");
        for (uint32_t k = 0; k < 4u; k++) {
            for (uint32_t block = 0; block < 16u && used > 0 &&
                                    (size_t)used < sizeof(scan_line); block++) {
                used += snprintf(scan_line + used, sizeof(scan_line) - (size_t)used,
                                 " v%u_b%02u=0x%x", (unsigned int)k, (unsigned int)block,
                                 (unsigned int)scan_counts[k][block]);
            }
            if (used > 0 && (size_t)used < sizeof(scan_line)) {
                used += snprintf(scan_line + used, sizeof(scan_line) - (size_t)used,
                                 " v%u_first=0x%x", (unsigned int)k, (unsigned int)scan_first[k]);
            }
        }
        if (used > 0 && (size_t)used + 2u < sizeof(scan_line)) {
            used += snprintf(scan_line + used, sizeof(scan_line) - (size_t)used, "\n");
            probe_emit_durable(emulated, scan_line, (size_t)used);
        }
    }
#endif

    probe_step(emulated, "vram-mirror", "restore-vram");
    vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 1);
    memcpy((void *)(uintptr_t)0x44000000u, backup, VRAM_MIRROR_SIZE);
    vram_mirror_cache_range(0u, VRAM_MIRROR_SIZE, 0);
    const volatile uint8_t *restored =
        (const volatile uint8_t *)(uintptr_t)0x44000000u;
    for (uint32_t byte = 0; byte < VRAM_MIRROR_SIZE; byte++) {
        restore_mismatches += restored[byte] != backup[byte];
    }
    const int restore_ok = restore_mismatches == 0u && ge_idle;
    const uint32_t restore_out[5] = {
        0x44000000u, VRAM_MIRROR_SIZE,
        2u * VRAM_MIRROR_CACHED_ALIAS_COUNT,
        restore_mismatches, ge_idle ? 1u : 0u,
    };
    defer_record("vram-restore", restore_ok ? "PASS" : "FAIL",
                 restore_ok ? 0u : 1u, restore_out, 5);
    free(backup);
    const uint32_t done = VRAM_MIRROR_SEMANTIC_RECORDS;
    defer_record("vram-mirror-done", restore_ok ? "PASS" : "FAIL",
                 restore_ok ? 0u : 1u, &done, 1);
    flush_deferred(emulated, "PSP-VRAM-MIRROR-001");
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
/* Interrupt-context proof from user mode. This probe is a user-mode module, so
   it cannot import InterruptManagerForKernel (sceKernelIsIntrContext): the PSP
   refuses to load a user-mode PRX that imports a kernel-only library. The proof
   is by construction instead: each trial is written by oracle_subintr_handler,
   which only the VBLANK sub-interrupt dispatch calls. The handler also records
   the raw return of sceKernelIsCpuIntrEnable() (the user Kernel_Library export
   from pspintrman.h) as context only.
   Pass rule (machinery only, corrected 2026-10-15): a trial is PASS with result 1
   when the handler reached the end of its body, so all six mutex cells were
   written and the trial was stored. No returned value is compared with an
   expected value in this probe or in the host checker; the conformance matrix in
   src/rt/intr_conformance.h compares them with measured cells. The corrected
   probe is re-measured at the next PSP session. */

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
    uint32_t done;        /* set last: every cell above is written and the trial is stored */
} IntrTrial;

static volatile int s_subintr_count;
static volatile SceUID s_intr_m_unlocked;   /* unlocked mutex (main does NOT hold) */
static volatile SceUID s_intr_m_main_owned; /* locked mutex owned by main thread    */
static IntrTrial s_trials[MUTEX_INTR_TRIALS];

/* VBLANK sub-interrupt handler.  Fires once per vertical blank (~60 Hz).
   Increments s_subintr_count only after writing all six cell results so the
   main thread can use s_subintr_count as the ready sentinel.  Records
   sceKernelIsCpuIntrEnable() as the first call, before any mutex operation,
   as context only. */
static int oracle_subintr_handler(int subintr, void *arg) {
    (void)subintr;
    (void)arg;
    int idx = s_subintr_count;
    if (idx >= MUTEX_INTR_TRIALS) {
        return 0;
    }
    IntrTrial t;
    /* Context only: the value is recorded, and the pass rule does not read it. */
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
    t.done = 1u;
    s_trials[idx] = t;
    /* Barrier: write count last so main only reads a complete trial. */
    s_subintr_count = idx + 1;
    return 0;
}

/* Emit one protocol record per trial.
   case_id format: mutex-interrupt-context-tNN (NN = zero-padded trial index).
   out0 = intr_enabled (context only), out1..out6 = six cell raw return values.
   Machinery-only verdict: PASS with result 1 when the handler completed the trial.
   The returned values are not compared with any expected value here. */
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
    /* pass iff the handler completed the trial: every cell above is written and the trial is stored. */
    int pass = (t->done == 1u);
    emit_mutex_test(emulated, case_id, pass, pass ? 1u : 0u, out, 7);
}

static uint32_t run_mutex_interrupt_context_case(int emulated) {
    s_subintr_count = 0;
    for (int i = 0; i < MUTEX_INTR_TRIALS; i++) {
        IntrTrial z = {0, 0, 0, 0, 0, 0, 0, 0};
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

    /* Emit header record: raw register and enable returns as context, then the completed count. */
    {
        uint32_t hdr[3];
        hdr[0] = (uint32_t)reg;
        hdr[1] = (uint32_t)ena;
        hdr[2] = (uint32_t)s_subintr_count;
        int pass = (s_subintr_count == MUTEX_INTR_TRIALS);
        emit_mutex_test(emulated, "mutex-interrupt-context", pass, pass ? 1u : 0u,
                        hdr, 3);
    }

    /* Emit per-trial records. */
    int n = s_subintr_count;
    if (n > MUTEX_INTR_TRIALS) n = MUTEX_INTR_TRIALS;
    for (int i = 0; i < n; i++) {
        emit_intr_trial(emulated, i, (const IntrTrial *)&s_trials[i]);
    }

    return (uint32_t)(s_subintr_count == MUTEX_INTR_TRIALS);
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

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_LATE_PRX_TARGET
/* #1106 late-PRX target. Every object below belongs to this module. The waiter blocks on the
   semaphore for the whole measurement window (600 s bound), so it never runs module code on its own. */
#define LATE_PRX_WAIT_US (600u * 1000u * 1000u)
static volatile SceUID s_late_sema = -1;
static volatile SceUID s_late_flag = -1;
static volatile SceUID s_late_cb = -1;
static volatile SceUID s_late_waiter = -1;

static int late_prx_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    return 0;
}

static int late_prx_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = LATE_PRX_WAIT_US;
    (void)sceKernelWaitSema(s_late_sema, 1, &timeout);
    for (;;) sceKernelSleepThread();
    return 0;
}

static void run_late_prx_target(int emulated) {
    s_late_sema = sceKernelCreateSema("nk_late_sema", 0, 0, 1, NULL);
    s_late_flag = sceKernelCreateEventFlag("nk_late_flag", 0, 0, NULL);
    s_late_cb = sceKernelCreateCallback("nk_late_cb", late_prx_callback, NULL);
    s_late_waiter = sceKernelCreateThread("nk_late_waiter", late_prx_waiter, 0x20, 0x1000,
                                          PSP_THREAD_ATTR_USER, NULL);
    if (s_late_waiter >= 0) (void)sceKernelStartThread(s_late_waiter, 0, NULL);
    /* Let the waiter reach its semaphore wait before the UIDs are published. */
    sceKernelDelayThread(500 * 1000);
    char line[320];
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-LATE-PRX-001 case_id=late-prx-published "
             "status=PASS sema=0x%08x flag=0x%08x cb=0x%08x waiter=0x%08x main=0x%08x\n",
             (unsigned int)s_late_sema, (unsigned int)s_late_flag, (unsigned int)s_late_cb,
             (unsigned int)s_late_waiter, (unsigned int)s_probe_main_thread);
    emit(emulated, line);
    /* Park the main thread. Returning would end main and drop PSPLink to the XMB. */
    for (;;) sceKernelSleepThread();
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_LATE_PRX_TARGET
/* Leave every owned object in place: the host measures the post-stop state. */
static int late_prx_module_stop(void) {
    return 0;
}
#endif

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_LATE_PRX_TARGET
    return late_prx_module_stop();
#endif
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

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_LLSC_LINK
/* ll/sc link facts (main thread only, no threads created). Each cell is one
   link window: ll loads a word and sets the link, then one or two sc run in the
   same asm block, with no call, branch, syscall or memory access between ll and
   the last sc. Every word a cell touches is written with its sentinel before the
   window opens, so a first-touch exception cannot fall inside it. CPU interrupts
   are suspended around each window (sceKernelCpuSuspendIntr, the user-mode call
   the display probes already use), so no interrupt return can clear the link
   inside it. No cell waits on time or on another thread: the cells are
   single-threaded register and memory facts.

   The expected words are the project's model (src/rt/guest_interp.c: sc stores
   and reports 1 only while the link is set, leaves the link as found, and does
   not compare its address with ll's). That follows the MIPS32 operation text; a
   PSP-3000 measured all three cells on 2026-10-15 (PSP-LLSC-001,
   docs/HARDWARE_ORACLE.md) and agrees. result is the sc result of the last sc, out0 the
   word ll loaded, out1 the result of the first sc, out2 and out3 the words at A
   and B after the cell, and out4 and out5 the model words for A and B; the model
   reports 1 for every sc. Whether the console agrees with the model is read from
   those words, not from the status, as in the fpu-vector family: the campaign
   runner accepts a capture only when every record is PASS, so a disagreement
   must not fail the record that measured it. The status says the window ran as
   designed. llsc-control is the control and must store (ll loaded A, the sc
   reported 1, A holds the stored word and B is untouched): if a plain pair does
   not store, the other two cells mean nothing. llsc-second-sc passes when ll
   loaded A and its first sc succeeded, which is the premise of the cell; what
   the second sc then did is the measurement. llsc-other-address passes when ll
   loaded A; what the sc to B did is the measurement.

   Cells: llsc-control (ll A; sc A), llsc-second-sc (ll A; sc A; sc A again with
   no new ll), and llsc-other-address (ll A; sc B). */
#define LLSC_LINK_CELLS 3u
#define LLSC_LINK_OUTS 6u
#define LLSC_LINK_A_INIT 0x11111111u
#define LLSC_LINK_B_INIT 0x44444444u
#define LLSC_LINK_FIRST 0x22222222u
#define LLSC_LINK_SECOND 0x33333333u
#define LLSC_LINK_OTHER 0x55555555u

/* Row c holds cell c's word A at [c][0] and word B at [c][1]; each row is one
   16-byte aligned block, so every ll and sc address is word aligned. */
static volatile uint32_t s_llsc_mem[LLSC_LINK_CELLS][4] __attribute__((aligned(16)));

/* One window: ll A, then sc A storing `value`. */
static uint32_t llsc_window_one(volatile uint32_t *a, uint32_t value, uint32_t *loaded) {
    uint32_t got;
    uint32_t result = value;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "ll %0, 0(%2)\n\t"
        "sc %1, 0(%2)\n\t"
        ".set pop\n\t"
        : "=&r"(got), "+r"(result)
        : "r"(a)
        : "memory");
    *loaded = got;
    return result;
}

/* One window: ll A, then two sc on A with no new ll between them. */
static void llsc_window_twice(volatile uint32_t *a, uint32_t first_value,
                              uint32_t second_value, uint32_t *loaded,
                              uint32_t *first, uint32_t *second) {
    uint32_t got;
    uint32_t r1 = first_value;
    uint32_t r2 = second_value;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "ll %0, 0(%3)\n\t"
        "sc %1, 0(%3)\n\t"
        "sc %2, 0(%3)\n\t"
        ".set pop\n\t"
        : "=&r"(got), "+r"(r1), "+r"(r2)
        : "r"(a)
        : "memory");
    *loaded = got;
    *first = r1;
    *second = r2;
}

/* One window: ll A, then sc B storing `value`. */
static uint32_t llsc_window_other(volatile uint32_t *a, volatile uint32_t *b,
                                  uint32_t value, uint32_t *loaded) {
    uint32_t got;
    uint32_t result = value;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "ll %0, 0(%2)\n\t"
        "sc %1, 0(%3)\n\t"
        ".set pop\n\t"
        : "=&r"(got), "+r"(result)
        : "r"(a), "r"(b)
        : "memory");
    *loaded = got;
    return result;
}

static void run_llsc_link(int emulated) {
    static const char *const names[LLSC_LINK_CELLS] = {
        "llsc-control", "llsc-second-sc", "llsc-other-address",
    };
    uint32_t loaded[LLSC_LINK_CELLS];
    uint32_t first[LLSC_LINK_CELLS];
    uint32_t last[LLSC_LINK_CELLS];
    uint32_t a_after[LLSC_LINK_CELLS];
    uint32_t b_after[LLSC_LINK_CELLS];
    const uint32_t model_a[LLSC_LINK_CELLS] = {
        LLSC_LINK_FIRST, LLSC_LINK_SECOND, LLSC_LINK_A_INIT,
    };
    const uint32_t model_b[LLSC_LINK_CELLS] = {
        LLSC_LINK_B_INIT, LLSC_LINK_B_INIT, LLSC_LINK_OTHER,
    };
    int token;

    /* Cell 0: a plain ll/sc pair, the control for the other two. */
    probe_step(emulated, "llsc-link", "llsc-control");
    s_llsc_mem[0][0] = LLSC_LINK_A_INIT;
    s_llsc_mem[0][1] = LLSC_LINK_B_INIT;
    token = sceKernelCpuSuspendIntr();
    first[0] = llsc_window_one(&s_llsc_mem[0][0], LLSC_LINK_FIRST, &loaded[0]);
    sceKernelCpuResumeIntr(token);
    last[0] = first[0];
    a_after[0] = s_llsc_mem[0][0];
    b_after[0] = s_llsc_mem[0][1];

    /* Cell 1: the second sc, with no new ll, after one that succeeded. */
    probe_step(emulated, "llsc-link", "llsc-second-sc");
    s_llsc_mem[1][0] = LLSC_LINK_A_INIT;
    s_llsc_mem[1][1] = LLSC_LINK_B_INIT;
    token = sceKernelCpuSuspendIntr();
    llsc_window_twice(&s_llsc_mem[1][0], LLSC_LINK_FIRST, LLSC_LINK_SECOND,
                      &loaded[1], &first[1], &last[1]);
    sceKernelCpuResumeIntr(token);
    a_after[1] = s_llsc_mem[1][0];
    b_after[1] = s_llsc_mem[1][1];

    /* Cell 2: sc to an address other than the one ll linked. */
    probe_step(emulated, "llsc-link", "llsc-other-address");
    s_llsc_mem[2][0] = LLSC_LINK_A_INIT;
    s_llsc_mem[2][1] = LLSC_LINK_B_INIT;
    token = sceKernelCpuSuspendIntr();
    first[2] = llsc_window_other(&s_llsc_mem[2][0], &s_llsc_mem[2][1],
                                 LLSC_LINK_OTHER, &loaded[2]);
    sceKernelCpuResumeIntr(token);
    last[2] = first[2];
    a_after[2] = s_llsc_mem[2][0];
    b_after[2] = s_llsc_mem[2][1];

    for (uint32_t c = 0; c < LLSC_LINK_CELLS; c++) {
        int pass = loaded[c] == LLSC_LINK_A_INIT;
        if (c == 0u) {
            pass = pass && first[0] == 1u && a_after[0] == model_a[0] &&
                   b_after[0] == model_b[0];
        } else if (c == 1u) {
            pass = pass && first[1] == 1u;
        }
        const uint32_t out[LLSC_LINK_OUTS] = {
            loaded[c], first[c], a_after[c], b_after[c], model_a[c], model_b[c],
        };
        emit_record_extended(emulated, "PSP-LLSC-001", names[c],
                             pass ? "PASS" : "FAIL", last[c], out, LLSC_LINK_OUTS);
    }
    uint32_t done = LLSC_LINK_CELLS;
    emit_record_extended(emulated, "PSP-LLSC-001", "llsc-link-done", "PASS",
                         0, &done, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_CC
/* COP1 condition codes (main thread only, no threads created). MIPS IV gives c.cond.s and
   bc1f/bc1t a 3-bit condition code (instruction bits 10..8 and 20..18) selecting FCC0..FCC7
   (FCR31 bit 23, then bits 25..31); MIPS II has one flag, FCC0, and the field reserved.
   The project's tools decide how to translate a nonzero condition code from what the PSP's
   FPU implements (issue #934 review). A PSP-3000 measured it on 2026-10-15 (PSP-FPU-CC-001,
   docs/HARDWARE_ORACLE.md): only FCC0 exists, bits 25..31 are not stored, and every
   condition code writes and tests FCC0. Every encoding below is emitted as a .word so the
   assembler cannot rewrite it.

   FCR31 is only ever written with condition bits (no trap enable, RM=RN), and is cleared
   before every cell and before exit. Each cell is emitted as soon as it ran, after a
   step marker, so a cell that raises an exception (a reserved condition-code encoding
   would) is named by the last STEP line and every earlier record stays durable.

   Records:
   - fpu-cc-fcr31-bits: result = FCR31 read back after writing 0xFE800000 (FCC0..FCC7 on
     MIPS IV); out0 = the word written; out1 = read back after writing 0x00800000 (FCC0 only).
   - fpu-cc<n>-ceq: c.eq.s with condition code n. result = FCR31 after an equal compare
     from FCR31 = 0; out0 = FCR31 after an unequal compare from FCR31 = 0xFE800000.
   - fpu-cc<n>-branch: taken flags (1 taken, 0 not) for bc1t with condition code n under
     FCR31 = FCC0 only (result), = 0 (out0), = FCCn only (out1; FCC0 only for n = 0), and
     for bc1f with condition code n under FCC0 only (out2) and 0 (out3).
   The n = 0 cells are the controls and must behave as MIPS defines condition code 0; the
   n = 1..7 cells are the measurement and pass once they ran. */
#define FPU_CC_FCC0 0x00800000u
#define FPU_CC_ALL 0xFE800000u

static inline uint32_t fpu_cc_get_fcr31(void) {
    uint32_t v;
    __asm__ volatile("cfc1 %0, $31" : "=r"(v) :: "memory");
    return v;
}

static inline void fpu_cc_set_fcr31(uint32_t v) {
    __asm__ volatile("ctc1 %0, $31\n\tnop\n\tnop" :: "r"(v) : "memory");
}

/* c.eq.s $f12, $f13 with condition code n, starting from FCR31 = start. */
#define FPU_CC_CEQ_FN(name, word)                                                    \
    static uint32_t name(uint32_t start, uint32_t a, uint32_t b) {                    \
        uint32_t after;                                                                \
        __asm__ volatile(".set push\n\t.set noreorder\n\t"                            \
                         "ctc1 %1, $31\n\t"                                           \
                         "mtc1 %2, $f12\n\t"                                          \
                         "mtc1 %3, $f13\n\t"                                          \
                         "nop\n\tnop\n\t"                                             \
                         ".word " #word "\n\t"                                        \
                         "nop\n\tnop\n\t"                                             \
                         "cfc1 %0, $31\n\t"                                           \
                         ".set pop\n\t"                                               \
                         : "=r"(after)                                                \
                         : "r"(start), "r"(a), "r"(b)                                 \
                         : "$f12", "$f13", "memory");                                 \
        return after;                                                                  \
    }

/* A bc1f/bc1t word with offset 2 skips the instruction after its delay slot: taken
   leaves the flag at 1, not taken clears it. */
#define FPU_CC_BRANCH_FN(name, word)                                                 \
    static uint32_t name(uint32_t fcr31) {                                             \
        uint32_t taken = 1u;                                                           \
        __asm__ volatile(".set push\n\t.set noreorder\n\t"                            \
                         "ctc1 %1, $31\n\t"                                           \
                         "nop\n\tnop\n\t"                                             \
                         ".word " #word "\n\t"                                        \
                         "nop\n\t"                                                    \
                         "move %0, $0\n\t"                                            \
                         ".set pop\n\t"                                               \
                         : "+r"(taken)                                                \
                         : "r"(fcr31)                                                 \
                         : "memory");                                                 \
        return taken;                                                                  \
    }

/* c.eq.s fs=$f12 ft=$f13: 0x460D6032 | (cc << 8). */
FPU_CC_CEQ_FN(fpu_cc0_ceq, 0x460D6032)
FPU_CC_CEQ_FN(fpu_cc1_ceq, 0x460D6132)
FPU_CC_CEQ_FN(fpu_cc2_ceq, 0x460D6232)
FPU_CC_CEQ_FN(fpu_cc3_ceq, 0x460D6332)
FPU_CC_CEQ_FN(fpu_cc4_ceq, 0x460D6432)
FPU_CC_CEQ_FN(fpu_cc5_ceq, 0x460D6532)
FPU_CC_CEQ_FN(fpu_cc6_ceq, 0x460D6632)
FPU_CC_CEQ_FN(fpu_cc7_ceq, 0x460D6732)
/* bc1t offset 2: 0x45010002 | (cc << 18); bc1f: 0x45000002 | (cc << 18). */
FPU_CC_BRANCH_FN(fpu_cc0_bc1t, 0x45010002)
FPU_CC_BRANCH_FN(fpu_cc1_bc1t, 0x45050002)
FPU_CC_BRANCH_FN(fpu_cc2_bc1t, 0x45090002)
FPU_CC_BRANCH_FN(fpu_cc3_bc1t, 0x450D0002)
FPU_CC_BRANCH_FN(fpu_cc4_bc1t, 0x45110002)
FPU_CC_BRANCH_FN(fpu_cc5_bc1t, 0x45150002)
FPU_CC_BRANCH_FN(fpu_cc6_bc1t, 0x45190002)
FPU_CC_BRANCH_FN(fpu_cc7_bc1t, 0x451D0002)
FPU_CC_BRANCH_FN(fpu_cc0_bc1f, 0x45000002)
FPU_CC_BRANCH_FN(fpu_cc1_bc1f, 0x45040002)
FPU_CC_BRANCH_FN(fpu_cc2_bc1f, 0x45080002)
FPU_CC_BRANCH_FN(fpu_cc3_bc1f, 0x450C0002)
FPU_CC_BRANCH_FN(fpu_cc4_bc1f, 0x45100002)
FPU_CC_BRANCH_FN(fpu_cc5_bc1f, 0x45140002)
FPU_CC_BRANCH_FN(fpu_cc6_bc1f, 0x45180002)
FPU_CC_BRANCH_FN(fpu_cc7_bc1f, 0x451C0002)

typedef uint32_t (*FpuCcCeqFn)(uint32_t, uint32_t, uint32_t);
typedef uint32_t (*FpuCcBranchFn)(uint32_t);

static void run_fpu_cc(int emulated) {
    static const FpuCcCeqFn ceq[8] = {
        fpu_cc0_ceq, fpu_cc1_ceq, fpu_cc2_ceq, fpu_cc3_ceq,
        fpu_cc4_ceq, fpu_cc5_ceq, fpu_cc6_ceq, fpu_cc7_ceq,
    };
    static const FpuCcBranchFn bc1t[8] = {
        fpu_cc0_bc1t, fpu_cc1_bc1t, fpu_cc2_bc1t, fpu_cc3_bc1t,
        fpu_cc4_bc1t, fpu_cc5_bc1t, fpu_cc6_bc1t, fpu_cc7_bc1t,
    };
    static const FpuCcBranchFn bc1f[8] = {
        fpu_cc0_bc1f, fpu_cc1_bc1f, fpu_cc2_bc1f, fpu_cc3_bc1f,
        fpu_cc4_bc1f, fpu_cc5_bc1f, fpu_cc6_bc1f, fpu_cc7_bc1f,
    };
    const uint32_t one = 0x3f800000u, two = 0x40000000u;
    uint32_t cells = 0;

    probe_step(emulated, "fpu-cc", "fpu-cc-fcr31-bits");
    fpu_cc_set_fcr31(FPU_CC_ALL);
    const uint32_t all_back = fpu_cc_get_fcr31();
    fpu_cc_set_fcr31(FPU_CC_FCC0);
    const uint32_t fcc0_back = fpu_cc_get_fcr31();
    fpu_cc_set_fcr31(0);
    {
        const uint32_t out[] = {FPU_CC_ALL, fcc0_back};
        emit_record_extended(emulated, "PSP-FPU-CC-001", "fpu-cc-fcr31-bits",
                             fcc0_back == FPU_CC_FCC0 ? "PASS" : "FAIL", all_back, out, 2);
        cells++;
    }

    for (uint32_t n = 0; n < 8u; n++) {
        char cid[32];
        snprintf(cid, sizeof(cid), "fpu-cc%u-ceq", (unsigned int)n);
        probe_step(emulated, "fpu-cc", cid);
        const uint32_t eq_from_zero = ceq[n](0u, one, one);
        fpu_cc_set_fcr31(0);
        const uint32_t ne_from_all = ceq[n](FPU_CC_ALL, one, two);
        fpu_cc_set_fcr31(0);
        int pass = 1;
        if (n == 0u) pass = eq_from_zero == FPU_CC_FCC0 && (ne_from_all & FPU_CC_FCC0) == 0u;
        {
            const uint32_t out[] = {ne_from_all};
            emit_record_extended(emulated, "PSP-FPU-CC-001", cid, pass ? "PASS" : "FAIL",
                                 eq_from_zero, out, 1);
            cells++;
        }

        snprintf(cid, sizeof(cid), "fpu-cc%u-branch", (unsigned int)n);
        probe_step(emulated, "fpu-cc", cid);
        const uint32_t own = n == 0u ? FPU_CC_FCC0 : (1u << (24u + n));
        const uint32_t t_fcc0 = bc1t[n](FPU_CC_FCC0);
        const uint32_t t_none = bc1t[n](0u);
        const uint32_t t_own = bc1t[n](own);
        const uint32_t f_fcc0 = bc1f[n](FPU_CC_FCC0);
        const uint32_t f_none = bc1f[n](0u);
        fpu_cc_set_fcr31(0);
        pass = 1;
        if (n == 0u) pass = t_fcc0 == 1u && t_none == 0u && f_fcc0 == 0u && f_none == 1u;
        {
            const uint32_t out[] = {t_none, t_own, f_fcc0, f_none};
            emit_record_extended(emulated, "PSP-FPU-CC-001", cid, pass ? "PASS" : "FAIL",
                                 t_fcc0, out, 4);
            cells++;
        }
    }
    fpu_cc_set_fcr31(0);
    emit_record_extended(emulated, "PSP-FPU-CC-001", "fpu-cc-done", "PASS", 0, &cells, 1);
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

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_FONT
/* Read-only walk of /DATA/FONT and /SYSPROFILE/RESOLUTION (PSP-REGISTRY-002). The walk, its
 * safety rules and its record format live in registry_walk.h, which a host harness also runs
 * against a fake registry (tools/test_psp_oracle.py). Records and step markers go through the
 * durable host0 writer; every walk buffer is static. */
#include "registry_walk.h"

static int s_registry_font_emulated;

static void registry_font_emit(void *ctx, const char *line, size_t length) {
    (void)ctx;
    probe_emit_durable(s_registry_font_emulated, line, length);
}

static void registry_font_step(void *ctx, const char *step) {
    (void)ctx;
    probe_step(s_registry_font_emulated, "registry-font", step);
}

static RegWalk s_registry_font_walk;

static void run_registry_font(int emulated) {
    s_registry_font_emulated = emulated;
    memset(&s_registry_font_walk, 0, sizeof(s_registry_font_walk));
    s_registry_font_walk.emit = registry_font_emit;
    s_registry_font_walk.step = registry_font_step;
    regwalk_run(&s_registry_font_walk);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_FILES
/* Read-only copy of flash0:/font to host0:/fonts (PSP-FONTFILES-001). The listing, copy and
 * verification live in font_files.h, which a host harness also runs against a fake file system
 * (tools/test_psp_oracle.py). Records and step markers go through the durable host0 writer; the
 * copies are opened with sceIoOpen directly so probe teardown never removes them. */
#include "font_files.h"

static int s_font_files_emulated;

static void font_files_emit(void *ctx, const char *line, size_t length) {
    (void)ctx;
    probe_emit_durable(s_font_files_emulated, line, length);
}

static void font_files_step(void *ctx, const char *step) {
    (void)ctx;
    probe_step(s_font_files_emulated, "font-files", step);
}

static FontFiles s_font_files;

static void run_font_files(int emulated) {
    s_font_files_emulated = emulated;
    memset(&s_font_files, 0, sizeof(s_font_files));
    s_font_files.emit = font_files_emit;
    s_font_files.step = font_files_step;
    fontfiles_run(&s_font_files);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_IO
/* Bare-device and disc open results (PSP-UMDIO-001). The fixed path lists and the calls live in
 * umd_io.h, which a host harness also runs (tools/test_psp_oracle.py). No UMD activation. */
#include "umd_io.h"

static int s_umd_io_emulated;

static void umd_io_emit(void *ctx, const char *line, size_t length) {
    (void)ctx;
    probe_emit_durable(s_umd_io_emulated, line, length);
}

static void umd_io_step(void *ctx, const char *step) {
    (void)ctx;
    probe_step(s_umd_io_emulated, "umd-io", step);
}

static UmdIo s_umd_io;

static void run_umd_io(int emulated) {
    s_umd_io_emulated = emulated;
    memset(&s_umd_io, 0, sizeof(s_umd_io));
    s_umd_io.emit = umd_io_emit;
    s_umd_io.step = umd_io_step;
    umdio_run(&s_umd_io);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_RAW_READ
/* Read-only raw UMD descriptor observations (PSP-UMDRAW-001), with the fixed cells and record
 * fields shared with the host fake-file-system harness. */
#include "umd_io.h"

static int s_umdraw_emulated;

static void umdraw_emit(void *ctx, const char *line, size_t length) {
    (void)ctx;
    probe_emit_durable(s_umdraw_emulated, line, length);
}

static void umdraw_step_callback(void *ctx, const char *step) {
    (void)ctx;
    probe_step(s_umdraw_emulated, "umd-raw-read", step);
}

static UmdRawRead s_umdraw;

static void run_umdraw(int emulated) {
    s_umdraw_emulated = emulated;
    memset(&s_umdraw, 0, sizeof(s_umdraw));
    s_umdraw.emit = umdraw_emit;
    s_umdraw.step = umdraw_step_callback;
    umdraw_run(&s_umdraw);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_LIST
/* The system font list as the console's own font library reports it (PSP-FONTLIST-001).
 * The calls, the arena allocator and the record format live in font_list.h, which a host
 * harness also runs against a fake library (tools/test_psp_oracle.py). The five sceLibFont
 * stubs come from libfont_imports.S; the build-time gate refuses any sceReg import in this
 * case, so the probe itself cannot open or create a registry category. Records and step
 * markers go through the durable host0 writer. */
#include "font_list.h"

static int s_font_list_emulated;

static void font_list_emit(void *ctx, const char *line, size_t length) {
    (void)ctx;
    probe_emit_durable(s_font_list_emulated, line, length);
}

static void font_list_step(void *ctx, const char *step) {
    (void)ctx;
    probe_step(s_font_list_emulated, "font-list", step);
}

static FontList s_font_list;

static void run_font_list(int emulated) {
    s_font_list_emulated = emulated;
    memset(&s_font_list, 0, sizeof(s_font_list));
    s_font_list.emit = font_list_emit;
    s_font_list.step = font_list_step;
    fontlist_run(&s_font_list);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_USERPGF
/* #1055: how the resident font library reads a project PGF (PSP-FONT-USERPGF-001).
 *
 * A title that runs its own sceLibFont renders every glyph of a project PGF as a one-pixel dash.
 * This case asks the library itself. For each source it opens the font, records the raw font
 * info, and for a handful of characters records the raw char info, the shadow char info for two
 * of them, and the glyph image the library draws into a zeroed 96x96 8-bit buffer (the non-zero
 * rows are recorded). The sources are, in order of risk: a console font (flash0:/font/ltn0.pgf,
 * opened read-only, from memory and by file name), the project nkltn.pgf as the converter
 * writes it (p0) and five variants (p1-p5, host0:/fup/), and finally sceFontOpen by index.
 * The variants change one hypothesis each: p1 sets the glyph role flag bit, p2 adds the
 * record's left/top, p3 changes the header resolution/size, p4 changes the header version,
 * vertical advance and shadow-map width, p5 applies all of them.
 *
 * The first project open in an earlier attempt never returned, so every job runs in its own
 * lower-priority thread under a progress watchdog: a job that makes no progress for
 * FUP_STALL_US is recorded as a hang (thread state, wait object, run clocks, the last step and
 * the library's allocation list), the first hang is held for FUP_HOLD_STEPS seconds so a person
 * can read the thread with PSPLink (the hold ends when host0:/fup/release appears), and the
 * hung thread is then terminated and the next job runs on a fresh library handle.
 *
 * The library must be resident (ldstart host0:/libfont.prx before the launch). Console font
 * bytes and glyph images are console data: the raw log stays private; only structural numbers
 * leave it. Nothing is written except the host0 log; flash0 is only opened PSP_O_RDONLY. */
#define FUP_TEST_ID "PSP-FONT-USERPGF-001"
#define FUP_SOURCES 7u
#define FUP_FILE_CAP (96u * 1024u)
#define FUP_ARENA_BYTES (2u * 1024u * 1024u)
#define FUP_ARENA_ALIGN 64u
#define FUP_ALLOC_RING 32u
#define FUP_GLYPH_SIDE 96u
#define FUP_BASE_X64 (16 * 64)
#define FUP_BASE_Y64 (64 * 64)
#define FUP_LINE_MAX 4096u
#define FUP_INFO_BYTES 512u
#define FUP_INFO_RECORD_BYTES 320u
#define FUP_CHAR_BYTES 128u
#define FUP_CHAR_RECORD_BYTES 80u
#define FUP_HEADER_BYTES 392u
#define FUP_PIXEL_FORMAT_8 2u
#define FUP_OPEN_MODE 1u
#define FUP_UNTOUCHED 0xa5a5a5a5u
#define FUP_STALL_US (12u * 1000u * 1000u)
#define FUP_JOB_MAX_US (180u * 1000u * 1000u)
#define FUP_POLL_US (200u * 1000u)
#define FUP_HOLD_STEPS 120
#define FUP_WORKER_PRIORITY 0x30
#define FUP_WORKER_STACK 0x10000
#define FUP_ERROR_WAIT_TIMEOUT 0x800201a8u

/* SceFontNewLibParams: eleven 32-bit words, as in font_list.h. */
typedef struct FupLibParams {
    void *userDataAddr;
    uint32_t numFonts;
    void *cacheDataAddr;
    void *(*allocFuncAddr)(void *data, uint32_t size);
    void (*freeFuncAddr)(void *data, void *block);
    void *openFuncAddr;
    void *closeFuncAddr;
    void *readFuncAddr;
    void *seekFuncAddr;
    void *errorFuncAddr;
    void *ioFinishFuncAddr;
} FupLibParams;
_Static_assert(sizeof(FupLibParams) == 11u * sizeof(void *), "SceFontNewLibParams is eleven slots");

/* SceFontImageBuffer / glyph image: 24 bytes on the 32-bit PSP. */
typedef struct FupGlyphImage {
    uint32_t pixelFormat;
    int32_t xPos64;
    int32_t yPos64;
    uint16_t bufWidth;
    uint16_t bufHeight;
    uint16_t bytesPerLine;
    uint16_t pad;
    uint8_t *buffer;
} FupGlyphImage;
_Static_assert(sizeof(FupGlyphImage) == 24u, "the glyph image keeps the 32-bit PSP layout");

uint32_t sceFontNewLib(FupLibParams *params, uint32_t *errorCode);
int sceFontDoneLib(uint32_t libHandle);
void *sceFontOpen(uint32_t libHandle, int index, uint32_t mode, int32_t *errorCode);
void *sceFontOpenUserFile(uint32_t libHandle, const char *fileName, uint32_t mode,
                          int32_t *errorCode);
void *sceFontOpenUserMemory(uint32_t libHandle, void *memory, uint32_t size, int32_t *errorCode);
int sceFontClose(void *fontHandle);
int sceFontGetFontInfo(void *fontHandle, void *fontInfo);
int sceFontGetCharInfo(void *fontHandle, uint32_t charCode, void *charInfo);
int sceFontGetCharGlyphImage(void *fontHandle, uint32_t charCode, FupGlyphImage *image);
int sceFontGetShadowInfo(void *fontHandle, uint32_t charCode, void *charInfo);

typedef struct FupSource {
    const char *tag;
    const char *path;
    uint32_t size;
    int32_t rc;
} FupSource;

enum { FUP_JOB_MEMORY = 0, FUP_JOB_FILE = 1, FUP_JOB_INDEX = 2 };

typedef struct FupJob {
    const char *tag;
    int kind;
    unsigned source;
    const char *path;
} FupJob;

static FupSource s_fup_sources[FUP_SOURCES] = {
    {"con", "flash0:/font/ltn0.pgf", 0, 0},
    {"p0", "host0:/fup/p0.pgf", 0, 0},
    {"p1", "host0:/fup/p1.pgf", 0, 0},
    {"p2", "host0:/fup/p2.pgf", 0, 0},
    {"p3", "host0:/fup/p3.pgf", 0, 0},
    {"p4", "host0:/fup/p4.pgf", 0, 0},
    {"p5", "host0:/fup/p5.pgf", 0, 0},
};

static const FupJob s_fup_jobs[] = {
    {"con-mem", FUP_JOB_MEMORY, 0, NULL},
    {"con-file", FUP_JOB_FILE, 0, "flash0:/font/ltn0.pgf"},
    {"p0-mem", FUP_JOB_MEMORY, 1, NULL},
    {"p1-mem", FUP_JOB_MEMORY, 2, NULL},
    {"p2-mem", FUP_JOB_MEMORY, 3, NULL},
    {"p3-mem", FUP_JOB_MEMORY, 4, NULL},
    {"p4-mem", FUP_JOB_MEMORY, 5, NULL},
    {"p5-mem", FUP_JOB_MEMORY, 6, NULL},
    {"p0-file", FUP_JOB_FILE, 1, "host0:/fup/p0.pgf"},
    {"con-idx", FUP_JOB_INDEX, 0, NULL},
};

static const uint32_t s_fup_codes[] = {0x41u, 0x67u, 0x2du, 0x31u, 0x20u, 0xe9u};
#define FUP_CODES (sizeof(s_fup_codes) / sizeof(s_fup_codes[0]))

typedef struct FupLibState {
    uint32_t alloc_calls;
    uint32_t alloc_refused;
    uint32_t free_calls;
    uint32_t top;
    uint32_t ring_size[FUP_ALLOC_RING];
    uint32_t ring_offset[FUP_ALLOC_RING];
} FupLibState;

static int s_fup_emulated;
static uint32_t s_fup_console_index = 1u;
static uint8_t s_fup_data[FUP_SOURCES][FUP_FILE_CAP] __attribute__((aligned(64)));
static uint8_t s_fup_arena[FUP_ARENA_BYTES] __attribute__((aligned(64)));
static uint8_t s_fup_info[FUP_INFO_BYTES] __attribute__((aligned(64)));
static uint8_t s_fup_char[FUP_CHAR_BYTES] __attribute__((aligned(64)));
static uint8_t s_fup_pixels[FUP_GLYPH_SIDE * FUP_GLYPH_SIDE] __attribute__((aligned(64)));
static char s_fup_line[2][FUP_LINE_MAX];
static char s_fup_step_name[96];
static FupLibState s_fup_lib;
static volatile uint32_t s_fup_progress;
static volatile int s_fup_done;

static void fup_record(unsigned which, const char *case_id, const char *status, uint32_t result,
                       const uint32_t *out, size_t out_count, const uint8_t *raw,
                       size_t raw_length) {
    static const char digits[] = "0123456789abcdef";
    char *line = s_fup_line[which];
    int used = snprintf(line, FUP_LINE_MAX,
                        "NAKAGAWA_PSP_TEST schema=1 test_id=" FUP_TEST_ID
                        " case_id=%s status=%s result=0x%08x",
                        case_id, status, (unsigned int)result);
    if (used <= 0 || (size_t)used >= FUP_LINE_MAX) return;
    for (size_t i = 0; i < out_count; i++) {
        const int more = snprintf(line + used, FUP_LINE_MAX - (size_t)used, " out%u=0x%08x",
                                  (unsigned int)i, (unsigned int)out[i]);
        if (more <= 0 || (size_t)(used + more) >= FUP_LINE_MAX) return;
        used += more;
    }
    if (raw && raw_length) {
        const int more = snprintf(line + used, FUP_LINE_MAX - (size_t)used, " raw_hex=");
        if (more <= 0 || (size_t)(used + more) + raw_length * 2u + 2u >= FUP_LINE_MAX) return;
        used += more;
        for (size_t i = 0; i < raw_length; i++) {
            line[used++] = digits[raw[i] >> 4];
            line[used++] = digits[raw[i] & 0xfu];
        }
    }
    line[used++] = '\n';
    line[used] = '\0';
    s_fup_progress++;
    probe_emit_durable(s_fup_emulated, line, (size_t)used);
}

static void fup_step(const char *name) {
    size_t n = 0;
    while (name[n] && n + 1u < sizeof(s_fup_step_name)) {
        s_fup_step_name[n] = name[n];
        n++;
    }
    s_fup_step_name[n] = '\0';
    s_fup_progress++;
    probe_step(s_fup_emulated, "font-userpgf", name);
}

static void *fup_alloc(void *data, uint32_t size) {
    FupLibState *state = (FupLibState *)data;
    const uint32_t want = size ? size : FUP_ARENA_ALIGN;
    const uint32_t aligned = (want + FUP_ARENA_ALIGN - 1u) & ~(FUP_ARENA_ALIGN - 1u);
    const uint32_t slot = state->alloc_calls;
    state->alloc_calls++;
    if (slot < FUP_ALLOC_RING) state->ring_size[slot] = size;
    if (aligned < want || aligned > FUP_ARENA_BYTES - state->top) {
        state->alloc_refused++;
        return NULL;
    }
    void *block = s_fup_arena + state->top;
    if (slot < FUP_ALLOC_RING) state->ring_offset[slot] = state->top;
    state->top += aligned;
    return block;
}

static void fup_free(void *data, void *block) {
    (void)block;
    ((FupLibState *)data)->free_calls++;
}

/* The library's allocation list as it stands: (size, offset) pairs, first 12 requests. */
static void fup_record_allocs(unsigned which, const char *tag) {
    char case_id[64];
    uint32_t out[4 + 2 * 12];
    uint32_t n = s_fup_lib.alloc_calls < 12u ? s_fup_lib.alloc_calls : 12u;
    out[0] = s_fup_lib.alloc_calls;
    out[1] = s_fup_lib.alloc_refused;
    out[2] = s_fup_lib.free_calls;
    out[3] = s_fup_lib.top;
    for (uint32_t i = 0; i < n; i++) {
        out[4 + 2 * i] = s_fup_lib.ring_size[i];
        out[5 + 2 * i] = s_fup_lib.ring_offset[i];
    }
    snprintf(case_id, sizeof(case_id), "fup-allocs-%s", tag);
    fup_record(which, case_id, "INFO", 0u, out, 4u + 2u * n, NULL, 0u);
}

static void fup_read_sources(void) {
    char case_id[64];
    for (unsigned i = 0; i < FUP_SOURCES; i++) {
        FupSource *source = &s_fup_sources[i];
        uint32_t out[3] = {0u, 0u, 0u};
        const SceUID fd = sceIoOpen(source->path, PSP_O_RDONLY, 0);
        if (fd < 0) {
            source->rc = fd;
            out[0] = (uint32_t)fd;
        } else {
            uint32_t total = 0;
            for (;;) {
                if (total >= FUP_FILE_CAP) {
                    source->rc = -2;
                    break;
                }
                const int got = sceIoRead(fd, s_fup_data[i] + total, FUP_FILE_CAP - total);
                if (got < 0) {
                    source->rc = got;
                    break;
                }
                if (got == 0) break;
                total += (uint32_t)got;
            }
            (void)sceIoClose(fd);
            source->size = total;
            out[0] = (uint32_t)source->rc;
            out[1] = total;
        }
        snprintf(case_id, sizeof(case_id), "fup-file-%s", source->tag);
        fup_record(1u, case_id, source->rc == 0 ? "PASS" : "SKIP", (uint32_t)source->rc, out, 2u,
                   source->size >= FUP_HEADER_BYTES ? s_fup_data[i] : NULL,
                   source->size >= FUP_HEADER_BYTES ? FUP_HEADER_BYTES : 0u);
    }
    {
        uint8_t text[16] = {0};
        const SceUID fd = sceIoOpen("host0:/fup/console_index.txt", PSP_O_RDONLY, 0);
        if (fd >= 0) {
            const int got = sceIoRead(fd, text, sizeof(text) - 1u);
            (void)sceIoClose(fd);
            if (got > 0 && text[0] >= '0' && text[0] <= '9') {
                uint32_t value = 0;
                for (int i = 0; i < got && text[i] >= '0' && text[i] <= '9'; i++) {
                    value = value * 10u + (uint32_t)(text[i] - '0');
                }
                s_fup_console_index = value;
            }
        }
        const uint32_t out[2] = {s_fup_console_index, (uint32_t)fd};
        fup_record(1u, "fup-console-index", "INFO", 0u, out, 2u, NULL, 0u);
    }
}

static void fup_query_glyph(const char *tag, uint32_t code, void *font) {
    char case_id[96];
    char step[64];
    uint32_t out[8];

    memset(s_fup_char, 0xa5, sizeof(s_fup_char));
    snprintf(step, sizeof(step), "charinfo-%s-%04x", tag, (unsigned int)code);
    fup_step(step);
    int rc = sceFontGetCharInfo(font, code, s_fup_char);
    snprintf(case_id, sizeof(case_id), "fup-char-%s-%04x", tag, (unsigned int)code);
    fup_record(0u, case_id, "INFO", (uint32_t)rc, NULL, 0u, s_fup_char, FUP_CHAR_RECORD_BYTES);

    FupGlyphImage image;
    memset(&image, 0, sizeof(image));
    memset(s_fup_pixels, 0, sizeof(s_fup_pixels));
    image.pixelFormat = FUP_PIXEL_FORMAT_8;
    image.xPos64 = FUP_BASE_X64;
    image.yPos64 = FUP_BASE_Y64;
    image.bufWidth = FUP_GLYPH_SIDE;
    image.bufHeight = FUP_GLYPH_SIDE;
    image.bytesPerLine = FUP_GLYPH_SIDE;
    image.buffer = s_fup_pixels;
    snprintf(step, sizeof(step), "glyph-%s-%04x", tag, (unsigned int)code);
    fup_step(step);
    rc = sceFontGetCharGlyphImage(font, code, &image);

    uint32_t count = 0u, sum = 0u;
    uint32_t min_x = FUP_GLYPH_SIDE, min_y = FUP_GLYPH_SIDE, max_x = 0u, max_y = 0u;
    for (uint32_t y = 0; y < FUP_GLYPH_SIDE; y++) {
        for (uint32_t x = 0; x < FUP_GLYPH_SIDE; x++) {
            const uint8_t value = s_fup_pixels[y * FUP_GLYPH_SIDE + x];
            if (!value) continue;
            count++;
            sum += value;
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
        }
    }
    if (!count) min_x = min_y = max_x = max_y = 0u;
    out[0] = count;
    out[1] = min_x;
    out[2] = min_y;
    out[3] = max_x;
    out[4] = max_y;
    out[5] = sum;
    snprintf(case_id, sizeof(case_id), "fup-glyph-%s-%04x", tag, (unsigned int)code);
    fup_record(0u, case_id, "INFO", (uint32_t)rc, out, 6u, (const uint8_t *)&image, sizeof(image));
    if (count) {
        for (uint32_t y = min_y; y <= max_y; y++) {
            out[0] = y;
            out[1] = min_x;
            out[2] = max_x - min_x + 1u;
            snprintf(case_id, sizeof(case_id), "fup-row-%s-%04x-%02u", tag, (unsigned int)code,
                     (unsigned int)y);
            fup_record(0u, case_id, "INFO", 0u, out, 3u, s_fup_pixels + y * FUP_GLYPH_SIDE + min_x,
                       max_x - min_x + 1u);
        }
    }
    if (code == 0x41u || code == 0x2du) {
        memset(s_fup_char, 0xa5, sizeof(s_fup_char));
        snprintf(step, sizeof(step), "shadow-%s-%04x", tag, (unsigned int)code);
        fup_step(step);
        rc = sceFontGetShadowInfo(font, code, s_fup_char);
        snprintf(case_id, sizeof(case_id), "fup-shadow-%s-%04x", tag, (unsigned int)code);
        fup_record(0u, case_id, "INFO", (uint32_t)rc, NULL, 0u, s_fup_char, FUP_CHAR_RECORD_BYTES);
    }
}

static void fup_job_body(const FupJob *job) {
    char case_id[96];
    char step[64];
    uint32_t out[6];
    FupLibParams params;

    memset(&s_fup_lib, 0, sizeof(s_fup_lib));
    memset(s_fup_arena, 0, sizeof(s_fup_arena));
    memset(&params, 0, sizeof(params));
    params.userDataAddr = &s_fup_lib;
    params.numFonts = 2u;
    params.allocFuncAddr = fup_alloc;
    params.freeFuncAddr = fup_free;

    fup_step("newlib");
    uint32_t error = FUP_UNTOUCHED;
    const uint32_t lib = sceFontNewLib(&params, &error);
    out[0] = error;
    out[1] = s_fup_lib.alloc_calls;
    out[2] = s_fup_lib.top;
    snprintf(case_id, sizeof(case_id), "fup-newlib-%s", job->tag);
    fup_record(0u, case_id, lib ? "PASS" : "FAIL", lib, out, 3u, NULL, 0u);
    if (!lib) return;

    void *font = NULL;
    int32_t open_error = (int32_t)FUP_UNTOUCHED;
    uint32_t detail = 0u;
    snprintf(step, sizeof(step), "open-%s", job->tag);
    fup_step(step);
    if (job->kind == FUP_JOB_MEMORY) {
        detail = s_fup_sources[job->source].size;
        if (s_fup_sources[job->source].rc == 0 && detail) {
            font = sceFontOpenUserMemory(lib, s_fup_data[job->source], detail, &open_error);
        } else {
            open_error = 0;
        }
    } else if (job->kind == FUP_JOB_FILE) {
        font = sceFontOpenUserFile(lib, job->path, FUP_OPEN_MODE, &open_error);
    } else {
        detail = s_fup_console_index;
        font = sceFontOpen(lib, (int)s_fup_console_index, FUP_OPEN_MODE, &open_error);
    }
    out[0] = (uint32_t)open_error;
    out[1] = detail;
    out[2] = s_fup_lib.alloc_calls;
    out[3] = s_fup_lib.top;
    snprintf(case_id, sizeof(case_id), "fup-open-%s", job->tag);
    fup_record(0u, case_id, font ? "PASS" : "FAIL", (uint32_t)(uintptr_t)font, out, 4u, NULL, 0u);

    if (font) {
        memset(s_fup_info, 0xa5, sizeof(s_fup_info));
        snprintf(step, sizeof(step), "fontinfo-%s", job->tag);
        fup_step(step);
        const int info_rc = sceFontGetFontInfo(font, s_fup_info);
        snprintf(case_id, sizeof(case_id), "fup-info-%s", job->tag);
        fup_record(0u, case_id, "INFO", (uint32_t)info_rc, NULL, 0u, s_fup_info,
                   FUP_INFO_RECORD_BYTES);
        for (size_t i = 0; i < FUP_CODES; i++) fup_query_glyph(job->tag, s_fup_codes[i], font);
        snprintf(step, sizeof(step), "close-%s", job->tag);
        fup_step(step);
        const int close_rc = sceFontClose(font);
        out[0] = (uint32_t)close_rc;
        snprintf(case_id, sizeof(case_id), "fup-close-%s", job->tag);
        fup_record(0u, case_id, "INFO", (uint32_t)close_rc, out, 1u, NULL, 0u);
    }
    fup_record_allocs(0u, job->tag);
    snprintf(step, sizeof(step), "donelib-%s", job->tag);
    fup_step(step);
    const int done_rc = sceFontDoneLib(lib);
    out[0] = s_fup_lib.free_calls;
    snprintf(case_id, sizeof(case_id), "fup-donelib-%s", job->tag);
    fup_record(0u, case_id, "INFO", (uint32_t)done_rc, out, 1u, NULL, 0u);
}

static int fup_worker(SceSize args, void *argp) {
    (void)args;
    const FupJob *job = *(const FupJob *const *)argp;
    fup_job_body(job);
    s_fup_done = 1;
    return 0;
}

static int fup_release_present(void) {
    const SceUID fd = sceIoOpen("host0:/fup/release", PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    (void)sceIoClose(fd);
    return 1;
}

/* Runs one job in its own thread. Returns 1 when the job hung. */
static int fup_run_job(const FupJob *job, int hold_allowed) {
    char case_id[96];
    uint32_t out[12];
    int hung = 0;

    s_fup_done = 0;
    s_fup_progress = 0u;
    s_fup_step_name[0] = '\0';
    const SceUID thread = sceKernelCreateThread("nk_fup_job", fup_worker, FUP_WORKER_PRIORITY,
                                                FUP_WORKER_STACK,
                                                PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    snprintf(case_id, sizeof(case_id), "fup-job-%s", job->tag);
    out[0] = (uint32_t)thread;
    out[1] = (uint32_t)job->kind;
    fup_record(1u, case_id, thread >= 0 ? "START" : "FAIL", (uint32_t)thread, out, 2u, NULL, 0u);
    if (thread < 0) return 0;
    const FupJob *argument = job;
    const int start_rc = sceKernelStartThread(thread, sizeof(argument), &argument);
    if (start_rc < 0) {
        out[0] = (uint32_t)start_rc;
        snprintf(case_id, sizeof(case_id), "fup-start-%s", job->tag);
        fup_record(1u, case_id, "FAIL", (uint32_t)start_rc, out, 1u, NULL, 0u);
        (void)sceKernelDeleteThread(thread);
        return 0;
    }

    const uint64_t begin = sceKernelGetSystemTimeWide();
    uint64_t last_change = begin;
    uint32_t last_progress = s_fup_progress;
    int wait_rc = 0;
    for (;;) {
        SceUInt timeout = FUP_POLL_US;
        wait_rc = sceKernelWaitThreadEnd(thread, &timeout);
        if (wait_rc != (int)FUP_ERROR_WAIT_TIMEOUT) break;
        const uint64_t now = sceKernelGetSystemTimeWide();
        if (s_fup_progress != last_progress) {
            last_progress = s_fup_progress;
            last_change = now;
        }
        if (now - last_change > FUP_STALL_US || now - begin > FUP_JOB_MAX_US) {
            hung = 1;
            break;
        }
    }
    const uint32_t elapsed_ms = (uint32_t)((sceKernelGetSystemTimeWide() - begin) / 1000u);

    if (hung) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        const int info_rc = sceKernelReferThreadStatus(thread, &info);
        out[0] = (uint32_t)thread;
        out[1] = (uint32_t)info.status;
        out[2] = (uint32_t)info.waitType;
        out[3] = (uint32_t)info.waitId;
        out[4] = (uint32_t)info.runClocks.low;
        out[5] = (uint32_t)info.runClocks.hi;
        out[6] = (uint32_t)info.intrPreemptCount;
        out[7] = (uint32_t)info.threadPreemptCount;
        out[8] = (uint32_t)info.releaseCount;
        out[9] = last_progress;
        out[10] = (uint32_t)info_rc;
        out[11] = elapsed_ms;
        snprintf(case_id, sizeof(case_id), "fup-hang-%s", job->tag);
        fup_record(1u, case_id, "HANG", (uint32_t)thread, out, 12u,
                   (const uint8_t *)s_fup_step_name, strlen(s_fup_step_name));
        fup_record_allocs(1u, job->tag);
        if (hold_allowed) {
            out[0] = (uint32_t)thread;
            out[1] = FUP_HOLD_STEPS;
            snprintf(case_id, sizeof(case_id), "fup-hold-begin-%s", job->tag);
            fup_record(1u, case_id, "INFO", (uint32_t)thread, out, 2u, NULL, 0u);
            for (int step = 0; step < FUP_HOLD_STEPS && !fup_release_present(); step++) {
                sceKernelDelayThread(1000 * 1000);
            }
            snprintf(case_id, sizeof(case_id), "fup-hold-end-%s", job->tag);
            fup_record(1u, case_id, "INFO", 0u, NULL, 0u, NULL, 0u);
        }
        const int terminate_rc = sceKernelTerminateDeleteThread(thread);
        out[0] = (uint32_t)terminate_rc;
        snprintf(case_id, sizeof(case_id), "fup-terminate-%s", job->tag);
        fup_record(1u, case_id, "INFO", (uint32_t)terminate_rc, out, 1u, NULL, 0u);
    } else {
        out[0] = (uint32_t)wait_rc;
        out[1] = elapsed_ms;
        out[2] = s_fup_done;
        snprintf(case_id, sizeof(case_id), "fup-job-done-%s", job->tag);
        fup_record(1u, case_id, "DONE", (uint32_t)wait_rc, out, 3u, NULL, 0u);
        (void)sceKernelDeleteThread(thread);
    }
    return hung;
}

static void run_font_userpgf(int emulated) {
    uint32_t out[3];
    uint32_t hangs = 0u;
    int hold_used = 0;
    const size_t jobs = sizeof(s_fup_jobs) / sizeof(s_fup_jobs[0]);

    s_fup_emulated = emulated;
    probe_step(emulated, "font-userpgf", "read-sources");
    fup_read_sources();
    for (size_t i = 0; i < jobs; i++) {
        probe_step(emulated, "font-userpgf", s_fup_jobs[i].tag);
        if (fup_run_job(&s_fup_jobs[i], !hold_used)) {
            hangs++;
            hold_used = 1;
        }
    }
    out[0] = (uint32_t)jobs;
    out[1] = hangs;
    out[2] = (uint32_t)s_fup_console_index;
    fup_record(1u, "font-userpgf-done", hangs ? "DONE" : "PASS", hangs, out, 3u, NULL, 0u);
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

    /* Item 10: sceGeBreak(1) reset with no sceGeListDeQueue earlier in this case.
       The reset is the only control call made on this stalled list, so its result
       is not mixed with a dequeue's side effect.  The cancelled-list sync cells are
       read after this same reset, so they do not depend on a successful dequeue. */
    probe_step(emulated, "ge-break-continue", "reset-no-dequeue");
    qid = sceGeListEnQueue((const void *)(uintptr_t)list,
                           (void *)(uintptr_t)list, -1, NULL);
    memset(&param, 0, sizeof(param));
    const int reset_rc = qid >= 0 ? sceGeBreak(1, &param) : qid;
    uint32_t reset_out[2] = {(uint32_t)qid, (uint32_t)reset_rc};
    emit_ge_control(emulated, "ge-break-reset-no-dequeue",
                    qid >= 0 ? "PASS" : "SKIP", (uint32_t)reset_rc, reset_out, 2);

    list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
    draw_state = qid >= 0 ? sceGeDrawSync(1) : qid;
    list_out[0] = (uint32_t)qid;
    list_out[1] = (uint32_t)reset_rc;
    list_out[2] = (uint32_t)list_state;
    draw_out[0] = (uint32_t)qid;
    draw_out[1] = (uint32_t)reset_rc;
    draw_out[2] = (uint32_t)draw_state;
    const char *cancel_status = qid >= 0 ? "PASS" : "SKIP";
    emit_ge_control(emulated, "ge-list-sync-cancelled", cancel_status,
                    (uint32_t)list_state, list_out, 3);
    emit_ge_control(emulated, "ge-draw-sync-cancelled", cancel_status,
                    (uint32_t)draw_state, draw_out, 3);
    emit_ge_quiesce(emulated, "ge-quiesce-after-cancel");

    /* sceGeListDeQueue on a stalled list is a measured cell.  The 2026-10-10 and
       2026-10-15 captures (not acceptance-eligible) read BUSY (0x80000021) here,
       which is why the cancelled-list cells no longer depend on this dequeue.  The
       status records only that the call completed, whatever it returned, so this
       cell never gates another cell.  The quiesce afterwards resets any list left
       queued. */
    probe_step(emulated, "ge-break-continue", "dequeue-stalled-list");
    qid = sceGeListEnQueue((const void *)(uintptr_t)list,
                           (void *)(uintptr_t)list, -1, NULL);
    const int dequeue_rc = qid >= 0 ? sceGeListDeQueue(qid) : qid;
    list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
    draw_state = qid >= 0 ? sceGeDrawSync(1) : qid;
    uint32_t dequeue_out[4] = {(uint32_t)qid, (uint32_t)dequeue_rc,
                               (uint32_t)list_state, (uint32_t)draw_state};
    emit_ge_control(emulated, "ge-list-dequeue-stalled",
                    qid >= 0 ? "PASS" : "SKIP", (uint32_t)dequeue_rc, dequeue_out, 4);
    emit_ge_quiesce(emulated, "ge-quiesce-after-dequeue");

    uint32_t done = 13;
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

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_AT3_DECODE
#include <pspatrac3.h>
#include <psputility_avmodules.h>
/* #1096: one bounded AT3 decode of the 8192-byte host0:/at3-input-buffer.bin stream. Every risky call is
   preceded by a durable step marker and its return code is written as soon as it is known, so a launch that
   hangs names its last step. Codec type PSP_ATRAC_AT3 (0x1001 in the issue). The probe never returns from
   main, so PSPLink stays up. */
#define AT3_DECODE_INPUT_PATH "host0:/at3-input-buffer.bin"
#define AT3_DECODE_BYTES 8192u
#define AT3_DECODE_PCM_SAMPLES 4096u

static unsigned char s_at3_input[AT3_DECODE_BYTES] __attribute__((aligned(64)));
static unsigned short s_at3_pcm[AT3_DECODE_PCM_SAMPLES] __attribute__((aligned(64)));

static uint32_t at3_le16(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t at3_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void at3_result(int emulated, const char *step, int rc) {
    char line[200];
    const int used = snprintf(line, sizeof(line),
                              "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-%s "
                              "status=RC result=0x%08x\n",
                              step, (unsigned int)rc);
    if (used > 0 && (size_t)used < sizeof(line)) {
        probe_emit_durable(emulated, line, (size_t)used);
    }
}

/* More decodes than the 24 frames an 8192-byte prefix holds, so the call that runs out of data is seen too. */
#define AT3_DECODE_EXTRA_CALLS 30

static void at3_pcm_record(int emulated, const char *step, int rc, int samples, int end_flag, int remain) {
    uint32_t hash = 2166136261u;
    uint32_t peak = 0;
    const int count = samples > 0 && samples <= 1024 ? samples * 2 : 0;
    for (int i = 0; i < count; i++) {
        const uint16_t v = s_at3_pcm[i];
        hash = (hash ^ (v & 0xffu)) * 16777619u;
        hash = (hash ^ (v >> 8)) * 16777619u;
        const uint32_t mag = (v & 0x8000u) ? (uint32_t)(0x10000u - v) : v;
        if (mag > peak) peak = mag;
    }
    char line[320];
    (void)snprintf(line, sizeof(line),
                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-pcm-%s status=RC "
                   "result=0x%08x samples=%d end=%d remain=%d fnv=0x%08x peak=%u "
                   "s0=0x%04x s1=0x%04x s2=0x%04x s3=0x%04x s256=0x%04x s2000=0x%04x\n",
                   step, (unsigned int)rc, samples, end_flag, remain, (unsigned int)hash,
                   (unsigned int)peak, s_at3_pcm[0], s_at3_pcm[1], s_at3_pcm[2], s_at3_pcm[3],
                   s_at3_pcm[256], s_at3_pcm[2000]);
    probe_emit_durable(emulated, line, strlen(line));
}

static void at3_stream_queries(int emulated, int atrac_id) {
    int next = -1, loop_start = -1, loop_end = -1, remain = -1, end_sample = -1;
    const int rc_next = sceAtracGetNextSample(atrac_id, &next);
    const int rc_remain = sceAtracGetRemainFrame(atrac_id, &remain);
    const int rc_sound = sceAtracGetSoundSample(atrac_id, &end_sample, &loop_start, &loop_end);
    char line[320];
    (void)snprintf(line, sizeof(line),
                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-queries status=RC "
                   "result=0x00000000 rc_next=0x%08x next=%d rc_remain=0x%08x remain=%d "
                   "rc_sound=0x%08x end_sample=%d loop_start=%d loop_end=%d\n",
                   (unsigned int)rc_next, next, (unsigned int)rc_remain, remain, (unsigned int)rc_sound,
                   end_sample, loop_start, loop_end);
    probe_emit_durable(emulated, line, strlen(line));
}

/* #1296 / #1096 follow-up: how long one sceAtracDecodeData takes, and whether its caller gives up
   the CPU meanwhile. A lower-priority peer only counts while the caller is not runnable, so a
   count that advances across a decode means the caller blocked inside it. A 1 ms delay is the
   positive control. Call 0's full PCM is dumped to the private host0 log. */
#define AT3_PEER_PRIO_STEP 8
static volatile uint32_t s_at3_peer_count;
static volatile int s_at3_peer_stop;

static int at3_peer_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (!s_at3_peer_stop) s_at3_peer_count++;
    return 0;
}

static void at3_timing_line(int emulated, const char *step, int rc, uint32_t dt_us, uint32_t peer) {
    char line[200];
    (void)snprintf(line, sizeof(line),
                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-timing-%s status=RC "
                   "result=0x%08x dt_us=%u peer=%u\n",
                   step, (unsigned int)rc, (unsigned int)dt_us, (unsigned int)peer);
    probe_emit_durable(emulated, line, strlen(line));
}

static int at3_timed_decode(int emulated, const char *step, int atrac_id, int *samples, int *end_flag,
                            int *remain) {
    const uint32_t c0 = s_at3_peer_count;
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const int rc = sceAtracDecodeData(atrac_id, s_at3_pcm, samples, end_flag, remain);
    const uint32_t t1 = sceKernelGetSystemTimeLow();
    const uint32_t c1 = s_at3_peer_count;
    at3_timing_line(emulated, step, rc, t1 - t0, c1 - c0);
    return rc;
}

static SceUID at3_peer_start(int emulated) {
    const int self = sceKernelGetThreadCurrentPriority();
    int prio = self + AT3_PEER_PRIO_STEP;
    if (prio > 0x6f) prio = 0x6f;
    s_at3_peer_count = 0;
    s_at3_peer_stop = 0;
    SceUID peer = sceKernelCreateThread("at3_peer", at3_peer_thread, prio, 0x1000, THREAD_ATTR_USER, NULL);
    int start_rc = peer >= 0 ? sceKernelStartThread(peer, 0, NULL) : peer;
    if (peer >= 0 && start_rc < 0) {
        sceKernelDeleteThread(peer);
        peer = -1;
    }
    /* Positive control: a 1 ms delay gives the CPU away, so the peer must count across it. */
    uint32_t control = 0;
    if (peer >= 0) {
        const uint32_t c0 = s_at3_peer_count;
        sceKernelDelayThread(1000);
        control = s_at3_peer_count - c0;
    }
    char line[200];
    (void)snprintf(line, sizeof(line),
                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-peer status=RC "
                   "result=0x%08x self_prio=%d peer_prio=%d control_delay_us=1000 control_peer=%u\n",
                   (unsigned int)start_rc, self, prio, (unsigned int)control);
    probe_emit_durable(emulated, line, strlen(line));
    return peer;
}

static void at3_peer_stop(SceUID peer) {
    s_at3_peer_stop = 1;
    if (peer < 0) return;
    SceUInt join_us = 2000000u;
    if (sceKernelWaitThreadEnd(peer, &join_us) < 0) sceKernelTerminateDeleteThread(peer);
    else sceKernelDeleteThread(peer);
}

/* Call 0's full PCM as hex lines in the durable host0 log (a file the probe creates is removed at
   teardown): 16 interleaved s16 values per line, in order. */
static void at3_pcm0_dump(int emulated, int samples) {
    const int count = samples > 0 && samples <= 1024 ? samples * 2 : 0;
    for (int base = 0; base < count; base += 16) {
        char line[200];
        int n = snprintf(line, sizeof(line),
                         "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-pcm0-hex status=RC "
                         "result=0x00000000 at=%d v=", base);
        for (int k = 0; k < 16 && base + k < count && n > 0 && n < (int)sizeof(line) - 6; k++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%04x", (unsigned int)s_at3_pcm[base + k]);
        if (n > 0 && n < (int)sizeof(line) - 1) {
            line[n++] = '\n';
            line[n] = '\0';
            probe_emit_durable(emulated, line, (size_t)n);
        }
    }
}

static void run_at3_decode(int emulated) {
    int input_rc = -1;
    int input_read = 0;
    probe_step(emulated, "at3-decode", "read-input");
    SceUID fd = sceIoOpen(AT3_DECODE_INPUT_PATH, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        uint32_t total = 0;
        while (total < AT3_DECODE_BYTES) {
            const int n = sceIoRead(fd, s_at3_input + total, AT3_DECODE_BYTES - total);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        input_read = (int)total;
        input_rc = total == AT3_DECODE_BYTES ? 0 : -2;
        (void)sceIoClose(fd);
    } else {
        input_rc = fd;
    }
    at3_result(emulated, "input", input_rc);

    if (input_rc == 0) {
        char header[256];
        (void)snprintf(header, sizeof(header),
                       "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-header status=RC "
                       "result=0x00000000 fmt=0x%04x channels=%u rate=%u block_align=%u "
                       "fact_bytes=%u fact_samples=%u fact_delay=%u input_read=%d\n",
                       (unsigned int)at3_le16(s_at3_input + 20), (unsigned int)at3_le16(s_at3_input + 22),
                       (unsigned int)at3_le32(s_at3_input + 24), (unsigned int)at3_le16(s_at3_input + 32),
                       (unsigned int)at3_le32(s_at3_input + 56), (unsigned int)at3_le32(s_at3_input + 60),
                       (unsigned int)at3_le32(s_at3_input + 64), input_read);
        probe_emit_durable(emulated, header, strlen(header));
    }

    int avcodec_rc = -1;
    int atrac_rc = -1;
    int atrac_id = -1;
    int setdata_rc = -1;
    int decode_rc = -1;
    int samples = -1;
    int end_flag = -1;
    int remain_frames = -1;
    int release_rc = -1;
    int atrac_unload_rc = -1;
    int avcodec_unload_rc = -1;
    int atrac_loaded = 0;
    int avcodec_loaded = 0;

    if (input_rc == 0) {
        probe_step(emulated, "at3-decode", "load-avcodec");
        avcodec_rc = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
        at3_result(emulated, "load-avcodec", avcodec_rc);
        if (avcodec_rc == 0) {
            avcodec_loaded = 1;
            probe_step(emulated, "at3-decode", "load-atrac3plus");
            atrac_rc = sceUtilityLoadAvModule(PSP_AV_MODULE_ATRAC3PLUS);
            at3_result(emulated, "load-atrac3plus", atrac_rc);
            if (atrac_rc == 0) {
                atrac_loaded = 1;
                probe_step(emulated, "at3-decode", "get-atracid");
                atrac_id = sceAtracGetAtracID(PSP_ATRAC_AT3);
                at3_result(emulated, "get-atracid", atrac_id);
                if (atrac_id >= 0) {
                    probe_step(emulated, "at3-decode", "set-data");
                    setdata_rc = sceAtracSetData(atrac_id, s_at3_input, AT3_DECODE_BYTES);
                    at3_result(emulated, "set-data", setdata_rc);
                    const SceUID peer = at3_peer_start(emulated);
                    probe_step(emulated, "at3-decode", "decode-data");
                    decode_rc = at3_timed_decode(emulated, "decode0", atrac_id, &samples, &end_flag,
                                                 &remain_frames);
                    at3_result(emulated, "decode-data", decode_rc);
                    char decoded[200];
                    (void)snprintf(decoded, sizeof(decoded),
                                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-AT3-001 case_id=at3-decoded "
                                   "status=RC result=0x%08x samples=%d end=%d remain=%d\n",
                                   (unsigned int)decode_rc, samples, end_flag, remain_frames);
                    probe_emit_durable(emulated, decoded, strlen(decoded));
                    /* #1096 follow-up: later frames, PCM fingerprints and the stream-position queries. The PCM
                       bytes stay in the private host0 log as a hash plus a few leading samples. */
                    at3_pcm_record(emulated, "decode0", decode_rc, samples, end_flag, remain_frames);
                    at3_pcm0_dump(emulated, samples);
                    at3_stream_queries(emulated, atrac_id);
                    for (int more = 1; more <= AT3_DECODE_EXTRA_CALLS && decode_rc == 0 && end_flag == 0;
                         more++) {
                        char step[24];
                        int s2 = -1, e2 = -1, r2 = -1;
                        (void)snprintf(step, sizeof(step), "decode%d", more);
                        probe_step(emulated, "at3-decode", step);
                        memset(s_at3_pcm, 0, sizeof(s_at3_pcm));
                        decode_rc = at3_timed_decode(emulated, step, atrac_id, &s2, &e2, &r2);
                        at3_pcm_record(emulated, step, decode_rc, s2, e2, r2);
                        end_flag = e2;
                    }
                    at3_peer_stop(peer);
                    at3_stream_queries(emulated, atrac_id);
                    probe_step(emulated, "at3-decode", "release-atracid");
                    release_rc = sceAtracReleaseAtracID(atrac_id);
                    at3_result(emulated, "release-atracid", release_rc);
                }
                probe_step(emulated, "at3-decode", "unload-atrac3plus");
                atrac_unload_rc = sceUtilityUnloadAvModule(PSP_AV_MODULE_ATRAC3PLUS);
                at3_result(emulated, "unload-atrac3plus", atrac_unload_rc);
            }
            probe_step(emulated, "at3-decode", "unload-avcodec");
            avcodec_unload_rc = sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
            at3_result(emulated, "unload-avcodec", avcodec_unload_rc);
        }
    }
    (void)atrac_loaded;
    (void)avcodec_loaded;
    (void)setdata_rc;
    (void)decode_rc;
    (void)release_rc;
    (void)atrac_unload_rc;
    (void)avcodec_unload_rc;
    probe_step(emulated, "at3-decode", "park");
    /* Normal teardown: parks until PSPLink modstop, which ends main cleanly. Returning from main would
       drop PSPLink to the XMB. */
    probe_teardown(emulated);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FAKE_SUCCESS
#include <pspsuspend.h>
#include <psputility.h>
#include <psputility_osk.h>
/* The runtime answers sceKernelVolatileMemUnlock/Lock/TryLock, sceUtilityOskUpdate/ShutdownStart and
   scePowerUnregisterCallback without a measured PSP result in some states. This probe records the PSP's
   return for each state. Default launch (no dialog ever opens):
     1. VolatileMem: Lock, TryLock while held, a second thread's Lock and TryLock while held, a double Lock
        from one thread, and the unlock that follows. Every blocking call runs on a helper thread so main
        never blocks; the lock is left released.
     2. scePower: Register/Unregister matrix. Unregister is only ever called on a slot this probe registered
        a moment before, or on an out-of-range slot, so no other module's callback can be dropped.
     3. sceUtilityOsk with no keyboard ever opened: GetStatus, Update, ShutdownStart.
   The keyboard part (an OSK dialog that needs a person to finish it) is compiled in only with
   FAKE_SUCCESS_OSK_CYCLE=1 and must be run attended. */
#ifdef FAKE_SUCCESS_OSK_CYCLE
static uint16_t s_fake_desc[1];
static uint16_t s_fake_intext[1];
static uint16_t s_fake_outtext[16];
static SceUtilityOskData s_fake_data;
static SceUtilityOskParams s_fake_params;
#endif

static void fake_record(int emulated, const char *step, uint32_t rc) {
    char line[200];
    const int used = snprintf(line, sizeof(line),
                              "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FAKE-001 case_id=fake-%s "
                              "status=RC result=0x%08x\n",
                              step, (unsigned int)rc);
    if (used > 0 && (size_t)used < sizeof(line)) {
        probe_emit_durable(emulated, line, (size_t)used);
    }
}

/* Helper-thread plumbing shared by the VolatileMem cells. */
static volatile int s_fake_h_mode;      /* 1: Lock once, 2: TryLock once, 3: Lock twice */
static volatile int s_fake_h_stage;     /* 0 not started, 1 first call issued, 2 first returned, 3 second issued, 4 second returned */
static volatile int s_fake_h_rc1;
static volatile int s_fake_h_rc2;
static volatile uint32_t s_fake_h_ptr;

static int fake_helper(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    void *ptr = NULL;
    int size = 0;
    const int mode = s_fake_h_mode;
    s_fake_h_stage = 1;
    if (mode == 2) {
        s_fake_h_rc1 = sceKernelVolatileMemTryLock(0, &ptr, &size);
    } else {
        s_fake_h_rc1 = sceKernelVolatileMemLock(0, &ptr, &size);
    }
    s_fake_h_ptr = (uint32_t)(uintptr_t)ptr;
    s_fake_h_stage = 2;
    if (mode == 3) {
        s_fake_h_stage = 3;
        s_fake_h_rc2 = sceKernelVolatileMemLock(0, &ptr, &size);
        s_fake_h_stage = 4;
    }
    for (;;) sceKernelSleepThread();
    return 0;
}

static SceUID fake_start_helper(int mode) {
    s_fake_h_mode = mode;
    s_fake_h_stage = 0;
    s_fake_h_rc1 = 0x7ffffff0;
    s_fake_h_rc2 = 0x7ffffff0;
    s_fake_h_ptr = 0;
    const SceUID helper = sceKernelCreateThread("nk_fake_helper", fake_helper, 0x20, 0x2000,
                                                PSP_THREAD_ATTR_USER, NULL);
    if (helper >= 0) (void)sceKernelStartThread(helper, 0, NULL);
    return helper;
}

static void fake_helper_state(int emulated, const char *step) {
    char line[240];
    (void)snprintf(line, sizeof(line),
                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FAKE-001 case_id=fake-%s "
                   "status=RC result=0x%08x stage=%d rc1=0x%08x rc2=0x%08x ptr=0x%08x\n",
                   step, 0u, s_fake_h_stage, (unsigned int)s_fake_h_rc1,
                   (unsigned int)s_fake_h_rc2, (unsigned int)s_fake_h_ptr);
    probe_emit_durable(emulated, line, strlen(line));
}

static void fake_stop_helper(SceUID helper) {
    if (helper >= 0) (void)sceKernelTerminateDeleteThread(helper);
}

static void fake_volatile_cells(int emulated) {
    void *ptr = NULL;
    int size = 0;

    /* A. Lock, then TryLock from the owner while held. */
    probe_step(emulated, "fake-success", "vol-lock");
    fake_record(emulated, "vol-lock", (uint32_t)sceKernelVolatileMemLock(0, &ptr, &size));
    probe_step(emulated, "fake-success", "vol-trylock-held-same-thread");
    ptr = NULL;
    size = 0;
    fake_record(emulated, "vol-trylock-held-same-thread",
                (uint32_t)sceKernelVolatileMemTryLock(0, &ptr, &size));

    /* B. Another thread's TryLock while main holds the lock. */
    probe_step(emulated, "fake-success", "vol-trylock-held-other-thread");
    SceUID h = fake_start_helper(2);
    sceKernelDelayThread(300 * 1000);
    fake_helper_state(emulated, "vol-trylock-held-other-thread");
    fake_stop_helper(h);

    /* C. Another thread's blocking Lock while main holds the lock, then main unlocks. */
    probe_step(emulated, "fake-success", "vol-lock-held-other-thread");
    h = fake_start_helper(1);
    sceKernelDelayThread(300 * 1000);
    fake_helper_state(emulated, "vol-lock-held-other-thread-while-held");
    probe_step(emulated, "fake-success", "vol-unlock-owner");
    fake_record(emulated, "vol-unlock-owner", (uint32_t)sceKernelVolatileMemUnlock(0));
    sceKernelDelayThread(300 * 1000);
    fake_helper_state(emulated, "vol-lock-held-other-thread-after-unlock");
    /* The helper (if it got the lock) still owns it; release from main and record the result. */
    probe_step(emulated, "fake-success", "vol-unlock-after-helper");
    fake_record(emulated, "vol-unlock-after-helper", (uint32_t)sceKernelVolatileMemUnlock(0));
    fake_stop_helper(h);
    probe_step(emulated, "fake-success", "vol-unlock-clean-check");
    fake_record(emulated, "vol-unlock-clean-check", (uint32_t)sceKernelVolatileMemUnlock(0));

    /* D. One thread locks twice (the second Lock may refuse or block); main unlocks to release it. */
    probe_step(emulated, "fake-success", "vol-lock-twice-same-thread");
    h = fake_start_helper(3);
    sceKernelDelayThread(300 * 1000);
    fake_helper_state(emulated, "vol-lock-twice-same-thread-while-held");
    fake_record(emulated, "vol-lock-twice-unlock-1", (uint32_t)sceKernelVolatileMemUnlock(0));
    sceKernelDelayThread(300 * 1000);
    fake_helper_state(emulated, "vol-lock-twice-same-thread-after-unlock-1");
    fake_record(emulated, "vol-lock-twice-unlock-2", (uint32_t)sceKernelVolatileMemUnlock(0));
    fake_stop_helper(h);
    /* Leave the lock released whatever happened: an unlock that reports 0x800201ae means it is. */
    probe_step(emulated, "fake-success", "vol-final-clean-check");
    fake_record(emulated, "vol-final-clean-check", (uint32_t)sceKernelVolatileMemUnlock(0));
    fake_record(emulated, "vol-final-clean-check-2", (uint32_t)sceKernelVolatileMemUnlock(0));
}

static int fake_power_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    return 0;
}

static void fake_power_cells(int emulated) {
    const SceUID cb = sceKernelCreateCallback("nk_fake_power_cb", fake_power_callback, NULL);
    fake_record(emulated, "pw-create-callback", (uint32_t)cb);
    if (cb < 0) return;
    int own[16];
    char step[48];

    /* Auto slot (-1): register, unregister, unregister again (the slot is now empty). */
    probe_step(emulated, "fake-success", "pw-auto");
    const int auto_slot = scePowerRegisterCallback(-1, cb);
    fake_record(emulated, "pw-register-auto", (uint32_t)auto_slot);
    if (auto_slot >= 0 && auto_slot < 16) {
        fake_record(emulated, "pw-unregister-owned", (uint32_t)scePowerUnregisterCallback(auto_slot));
        fake_record(emulated, "pw-unregister-empty-after-own", (uint32_t)scePowerUnregisterCallback(auto_slot));
    }

    /* Explicit slots 0..15: registration shows which are free; only slots registered here are unregistered. */
    probe_step(emulated, "fake-success", "pw-slots");
    for (int slot = 0; slot < 16; slot++) {
        (void)snprintf(step, sizeof(step), "pw-register-slot%d", slot);
        const int rc = scePowerRegisterCallback(slot, cb);
        fake_record(emulated, step, (uint32_t)rc);
        own[slot] = rc >= 0;
    }
    for (int slot = 0; slot < 16; slot++) {
        if (!own[slot]) continue;
        (void)snprintf(step, sizeof(step), "pw-unregister-owned-slot%d", slot);
        fake_record(emulated, step, (uint32_t)scePowerUnregisterCallback(slot));
        (void)snprintf(step, sizeof(step), "pw-unregister-empty-slot%d", slot);
        fake_record(emulated, step, (uint32_t)scePowerUnregisterCallback(slot));
    }

    /* Range cells. Register side first; Unregister is called on an out-of-range slot only when the
       Register at that slot was refused (so this probe does not own it). */
    probe_step(emulated, "fake-success", "pw-range");
    const int reg16 = scePowerRegisterCallback(16, cb);
    fake_record(emulated, "pw-register-slot16", (uint32_t)reg16);
    const int reg32 = scePowerRegisterCallback(32, cb);
    fake_record(emulated, "pw-register-slot32", (uint32_t)reg32);
    const int regm2 = scePowerRegisterCallback(-2, cb);
    fake_record(emulated, "pw-register-slotm2", (uint32_t)regm2);
    if (reg16 >= 0) fake_record(emulated, "pw-unregister-owned-slot16", (uint32_t)scePowerUnregisterCallback(16));
    if (reg32 >= 0) fake_record(emulated, "pw-unregister-owned-slot32", (uint32_t)scePowerUnregisterCallback(32));
    if (regm2 >= 0) fake_record(emulated, "pw-unregister-owned-slotm2", (uint32_t)scePowerUnregisterCallback(-2));
    if (reg16 < 0) fake_record(emulated, "pw-unregister-slot16", (uint32_t)scePowerUnregisterCallback(16));
    if (reg32 < 0) fake_record(emulated, "pw-unregister-slot32", (uint32_t)scePowerUnregisterCallback(32));
    if (regm2 < 0) fake_record(emulated, "pw-unregister-slotm2", (uint32_t)scePowerUnregisterCallback(-2));
    fake_record(emulated, "pw-unregister-slotm1", (uint32_t)scePowerUnregisterCallback(-1));
    fake_record(emulated, "pw-unregister-slot1000", (uint32_t)scePowerUnregisterCallback(1000));
    (void)sceKernelDeleteCallback(cb);
}

#ifdef FAKE_SUCCESS_OSK_CYCLE
/* Attended only. The dialog needs a person: press the cancel or confirm control on the PSP. The wait is
   bounded; if the dialog is still open at the end the probe records that, and the caller must NOT soft
   reset the device (a visible keyboard before a soft reset wedged PSPLink on 2026-10-21). */
#define FAKE_OSK_WAIT_POLLS 3750 /* 60 s at 16 ms */
static void fake_osk_cycle(int emulated) {
    memset(&s_fake_params, 0, sizeof(s_fake_params));
    memset(&s_fake_data, 0, sizeof(s_fake_data));
    s_fake_params.base.size = sizeof(s_fake_params);
    s_fake_params.base.graphicsThread = 0x11;
    s_fake_params.base.accessThread = 0x13;
    s_fake_params.base.fontThread = 0x12;
    s_fake_params.base.soundThread = 0x10;
    s_fake_params.datacount = 1;
    s_fake_params.data = &s_fake_data;
    s_fake_data.lines = 1;
    s_fake_data.desc = s_fake_desc;
    s_fake_data.intext = s_fake_intext;
    s_fake_data.outtext = s_fake_outtext;
    s_fake_data.outtextlength = 16;
    s_fake_data.outtextlimit = 16;

    probe_step(emulated, "fake-success", "osk-initstart");
    const int init_rc = sceUtilityOskInitStart(&s_fake_params);
    fake_record(emulated, "osk-initstart", (uint32_t)init_rc);
    if (init_rc != 0) return;
    uint32_t status = 0xffffffffu;
    int visible_seen = 0;
    int quit_seen = 0;
    int polls = 0;
    for (polls = 0; polls < FAKE_OSK_WAIT_POLLS; polls++) {
        status = (uint32_t)sceUtilityOskGetStatus();
        if (status == 2u && !visible_seen) {
            visible_seen = 1;
            fake_record(emulated, "osk-update-visible-speed0", (uint32_t)sceUtilityOskUpdate(0));
            fake_record(emulated, "osk-update-visible-speed1", (uint32_t)sceUtilityOskUpdate(1));
            fake_record(emulated, "osk-shutdown-visible", (uint32_t)sceUtilityOskShutdownStart());
        }
        if (status == 2u) (void)sceUtilityOskUpdate(1);
        if (status == 3u) {
            quit_seen = 1;
            fake_record(emulated, "osk-update-quit", (uint32_t)sceUtilityOskUpdate(1));
            fake_record(emulated, "osk-shutdown-quit", (uint32_t)sceUtilityOskShutdownStart());
            break;
        }
        if ((status == 0u || status == 0x80110005u) && visible_seen) break;
        sceKernelDelayThread(16000);
    }
    char line[200];
    (void)snprintf(line, sizeof(line),
                   "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FAKE-001 case_id=fake-osk-status-end "
                   "status=RC result=0x%08x polls=%d visible=%d quit=%d\n",
                   (unsigned int)status, polls, visible_seen, quit_seen);
    probe_emit_durable(emulated, line, strlen(line));
    for (int i = 0; i < 200; i++) {
        const uint32_t now = (uint32_t)sceUtilityOskGetStatus();
        if (now == 0u || now == 0x80110005u) break;
        (void)sceUtilityOskUpdate(1);
        sceKernelDelayThread(16000);
    }
    const uint32_t final_status = (uint32_t)sceUtilityOskGetStatus();
    fake_record(emulated, "osk-final-status", final_status);
    if (final_status == 0u || final_status == 0x80110005u) {
        fake_record(emulated, "osk-update-none-after-use", (uint32_t)sceUtilityOskUpdate(0));
        fake_record(emulated, "osk-shutdown-none-after-use", (uint32_t)sceUtilityOskShutdownStart());
    }
}
#endif

static void run_fake_success(int emulated) {
    probe_step(emulated, "fake-success", "volatile-cells");
    fake_volatile_cells(emulated);
    probe_step(emulated, "fake-success", "power-cells");
    fake_power_cells(emulated);

    /* No keyboard has ever been opened by this launch: these cells never create a dialog. */
    probe_step(emulated, "fake-success", "osk-none-cells");
    fake_record(emulated, "osk-status-none", (uint32_t)sceUtilityOskGetStatus());
    fake_record(emulated, "osk-update-none-speed0", (uint32_t)sceUtilityOskUpdate(0));
    fake_record(emulated, "osk-update-none-speed1", (uint32_t)sceUtilityOskUpdate(1));
    fake_record(emulated, "osk-shutdown-none", (uint32_t)sceUtilityOskShutdownStart());
    fake_record(emulated, "osk-status-none-after", (uint32_t)sceUtilityOskGetStatus());
#ifdef FAKE_SUCCESS_OSK_CYCLE
    probe_step(emulated, "fake-success", "osk-cycle");
    fake_osk_cycle(emulated);
#endif
    probe_step(emulated, "fake-success", "done");
    fake_record(emulated, "done", 0);
    /* Normal teardown: parks until PSPLink modstop, which ends main cleanly. */
    probe_teardown(emulated);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING
#define CPU_TIMING_TRIALS 7u
#define CPU_TIMING_ROUNDS 8u
#define CPU_TIMING_OPS 8u
#define CPU_TIMING_MISS_MAX_BLOCKS 256u
#define CPU_TIMING_MISS_STRIDE 512u
#define CPU_TIMING_MISS_BYTES (CPU_TIMING_MISS_MAX_BLOCKS * CPU_TIMING_MISS_STRIDE)
#define CPU_TIMING_SYSCALL_CALLS 2048u
#define CPU_TIMING_DRAW_SYNC_CALLS 4u
#define CPU_TIMING_GE_CALLS_PER_TRIAL 1u
#define CPU_TIMING_RESULT_COUNT 65u
#define CPU_TIMING_REPEAT_8(OP) OP OP OP OP OP OP OP OP

typedef uint32_t (*CpuTimingLoop)(uint32_t blocks);
typedef int (*CpuTimingCall)(void);
typedef struct {
    uint32_t calls;
    uint32_t elapsed[CPU_TIMING_TRIALS];
    uint32_t control[CPU_TIMING_TRIALS];
    int error;
} CpuTimingDrawSyncJob;

static uint32_t s_cpu_timing_memory[CPU_TIMING_MISS_BYTES / sizeof(uint32_t)]
    __attribute__((aligned(64)));
static uint32_t s_cpu_timing_ge_list[64] __attribute__((aligned(64)));
static uint32_t s_cpu_timing_ge_list_bytes;
static int s_cpu_timing_ge_qids[CPU_TIMING_GE_CALLS_PER_TRIAL];
static uint32_t s_cpu_timing_ge_count;
static uint32_t s_cpu_timing_ge_index;
static SceLwMutexWorkarea s_cpu_timing_lwmutex;
static SceUID s_cpu_timing_sema = -1;
static void *s_cpu_timing_framebuffer;
static int s_cpu_timing_stride;
static int s_cpu_timing_format;
static volatile uint32_t s_cpu_timing_sink;

static uint32_t cpu_timing_loop_empty(uint32_t blocks) {
    __asm__ volatile(
        ".set push\n\t.set noreorder\n\t"
        "1:\n\taddiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks) : : "memory");
    return blocks;
}

static uint32_t cpu_timing_loop_walk(uint32_t blocks) {
    uint32_t address = (uint32_t)(uintptr_t)s_cpu_timing_memory;
    __asm__ volatile(
        ".set push\n\t.set noreorder\n\t1:\n\t"
        "addiu %1,%1,512\n\taddiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "+&r"(address) : : "memory");
    return address;
}

#define CPU_TIMING_ALU_FN(NAME, OP) \
static uint32_t NAME(uint32_t blocks) { \
    uint32_t value = 0x13579bdfu; \
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t" \
        CPU_TIMING_REPEAT_8(OP) \
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t" \
        : "+&r"(blocks), "+&r"(value) : : "memory"); \
    return value; \
}

CPU_TIMING_ALU_FN(cpu_timing_alu, "addu %1,%1,$zero\n\t")
CPU_TIMING_ALU_FN(cpu_timing_shift, "sll %1,%1,1\n\t")
CPU_TIMING_ALU_FN(cpu_timing_logic, "xor %1,%1,$zero\n\t")

static uint32_t cpu_timing_mul_pair(uint32_t blocks) {
    const uint32_t operand = 0x12345u;
    uint32_t product = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("multu %2,%2\n\tmflo %1\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "=&r"(product) : "r"(operand) : "memory");
    return product;
}

static uint32_t cpu_timing_div_pair(uint32_t blocks) {
    const uint32_t operand = 0x12345u;
    uint32_t quotient = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("divu %2,%2\n\tmflo %1\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "=&r"(quotient) : "r"(operand) : "memory");
    return quotient;
}

static uint32_t cpu_timing_load_hit(uint32_t blocks) {
    uint32_t value = 0;
    const uint32_t address = (uint32_t)(uintptr_t)s_cpu_timing_memory;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("lw %1,0(%2)\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "=&r"(value) : "r"(address) : "memory");
    return value;
}

static uint32_t cpu_timing_store_hit(uint32_t blocks) {
    const uint32_t address = (uint32_t)(uintptr_t)s_cpu_timing_memory;
    const uint32_t value = 0x5a5aa5a5u;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("sw %2,0(%1)\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks) : "r"(address), "r"(value) : "memory");
    return blocks;
}

static uint32_t cpu_timing_load_miss(uint32_t blocks) {
    uint32_t address = (uint32_t)(uintptr_t)s_cpu_timing_memory;
    uint32_t value = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        "lw %1,0(%2)\n\tlw %1,64(%2)\n\tlw %1,128(%2)\n\tlw %1,192(%2)\n\t"
        "lw %1,256(%2)\n\tlw %1,320(%2)\n\tlw %1,384(%2)\n\tlw %1,448(%2)\n\t"
        "addiu %2,%2,512\n\taddiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "=&r"(value), "+&r"(address) : : "memory");
    return value;
}

static uint32_t cpu_timing_store_miss(uint32_t blocks) {
    uint32_t address = (uint32_t)(uintptr_t)s_cpu_timing_memory;
    const uint32_t value = 0x5a5aa5a5u;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        "sw %2,0(%1)\n\tsw %2,64(%1)\n\tsw %2,128(%1)\n\tsw %2,192(%1)\n\t"
        "sw %2,256(%1)\n\tsw %2,320(%1)\n\tsw %2,384(%1)\n\tsw %2,448(%1)\n\t"
        "addiu %1,%1,512\n\taddiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "+&r"(address) : "r"(value) : "memory");
    return address;
}

static uint32_t cpu_timing_branch_taken(uint32_t blocks) {
    uint32_t value = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("beq $zero,$zero,2f\n\tnop\n\t2:\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "+&r"(value) : : "memory");
    return value;
}

static uint32_t cpu_timing_branch_not_taken(uint32_t blocks) {
    uint32_t value = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("bne $zero,$zero,2f\n\tnop\n\t2:\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "+&r"(value) : : "memory");
    return value;
}

static uint32_t cpu_timing_branch_delay(uint32_t blocks) {
    uint32_t value = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t1:\n\t"
        CPU_TIMING_REPEAT_8("beq $zero,$zero,2f\n\taddiu %1,%1,1\n\t2:\n\t")
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\t.set pop\n\t"
        : "+&r"(blocks), "+&r"(value) : : "memory");
    return value;
}

#define CPU_TIMING_FP_FN(NAME, OP) \
static uint32_t NAME(uint32_t blocks) { \
    const uint32_t one = 0x3f800000u; \
    uint32_t result = 0; \
    __asm__ volatile(".set push\n\t.set noreorder\n\tmtc1 %2,$f0\n\tmtc1 %2,$f2\n\t1:\n\t" \
        CPU_TIMING_REPEAT_8(OP) \
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\tmfc1 %1,$f0\n\t.set pop\n\t" \
        : "+&r"(blocks), "=&r"(result) : "r"(one) : "memory"); \
    return result; \
}

CPU_TIMING_FP_FN(cpu_timing_fpu_add, "add.s $f0,$f0,$f2\n\t")
CPU_TIMING_FP_FN(cpu_timing_fpu_mul, "mul.s $f0,$f0,$f2\n\t")
CPU_TIMING_FP_FN(cpu_timing_fpu_div, "div.s $f0,$f0,$f2\n\t")

#define CPU_TIMING_VFPU_FN(NAME, OP) \
static uint32_t NAME(uint32_t blocks) { \
    const uint32_t one = 0x3f800000u; \
    uint32_t result = 0; \
    __asm__ volatile(".set push\n\t.set noreorder\n\tmtv %2,S000\n\tmtv %2,S001\n\t1:\n\t" \
        CPU_TIMING_REPEAT_8(OP) \
        "addiu %0,%0,-1\n\tbnez %0,1b\n\tnop\n\tmfv %1,S000\n\t.set pop\n\t" \
        : "+&r"(blocks), "=&r"(result) : "r"(one) : "memory"); \
    return result; \
}

CPU_TIMING_VFPU_FN(cpu_timing_vfpu_add, "vadd.s S000,S000,S001\n\t")
CPU_TIMING_VFPU_FN(cpu_timing_vfpu_mul, "vmul.s S000,S000,S001\n\t")

static void cpu_timing_sort(uint32_t *values) {
    for (uint32_t i = 1; i < CPU_TIMING_TRIALS; i++) {
        const uint32_t value = values[i];
        uint32_t j = i;
        while (j > 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = value;
    }
}

static uint32_t cpu_timing_elapsed(CpuTimingLoop fn, uint32_t blocks,
                                   uint32_t cold_cache) {
    if (cold_cache) sceKernelDcacheWritebackInvalidateAll();
    const uint64_t start = (uint64_t)sceKernelGetSystemTimeWide();
    for (uint32_t round = 0; round < CPU_TIMING_ROUNDS; round++)
        s_cpu_timing_sink ^= fn(blocks);
    return (uint32_t)((uint64_t)sceKernelGetSystemTimeWide() - start);
}

static void cpu_timing_emit_instruction(int emulated, const char *name,
                                        CpuTimingLoop fn, CpuTimingLoop control,
                                        uint32_t blocks, uint32_t ops_per_block,
                                        uint32_t cpu_mhz, int cold_cache) {
    uint32_t elapsed[CPU_TIMING_TRIALS], baseline[CPU_TIMING_TRIALS];
    (void)fn(1u);
    (void)control(1u);
    for (uint32_t trial = 0; trial < CPU_TIMING_TRIALS; trial++) {
        elapsed[trial] = cpu_timing_elapsed(fn, blocks, (uint32_t)cold_cache);
        baseline[trial] = cpu_timing_elapsed(control, blocks, 0u);
    }
    cpu_timing_sort(elapsed);
    cpu_timing_sort(baseline);
    const uint32_t measured_us = elapsed[CPU_TIMING_TRIALS / 2u];
    const uint32_t control_us = baseline[CPU_TIMING_TRIALS / 2u];
    const uint32_t operations = blocks * CPU_TIMING_ROUNDS * ops_per_block;
    const uint32_t delta_us = measured_us >= control_us ? measured_us - control_us : 0u;
    const uint32_t cycles_x100 = operations == 0u ? 0u :
        (uint32_t)(((uint64_t)delta_us * cpu_mhz * 100u + operations / 2u) / operations);
    uint32_t out[4] = {operations, measured_us, control_us, cpu_mhz};
    char case_id[48];
    snprintf(case_id, sizeof(case_id), "%s-%u", name, (unsigned int)blocks);
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", case_id,
                         measured_us >= control_us ? "PASS" : "INCONCLUSIVE",
                         cycles_x100, out, 4);
}

static int cpu_timing_noop_call(void) { return 0; }
static int cpu_timing_call_hcount(void) { return sceDisplayGetAccumulatedHcount(); }
static int cpu_timing_call_setframebuf(void) {
    return sceDisplaySetFrameBuf(s_cpu_timing_framebuffer, s_cpu_timing_stride,
                                 s_cpu_timing_format, PSP_DISPLAY_SETBUF_IMMEDIATE);
}
static int cpu_timing_call_sema_pair(void) {
    const int signal = sceKernelSignalSema(s_cpu_timing_sema, 1);
    SceUInt timeout = 1000u;
    return signal < 0 ? signal : sceKernelWaitSema(s_cpu_timing_sema, 1, &timeout);
}
static int cpu_timing_call_lwmutex_pair(void) {
    const int lock = sceKernelLockLwMutex(&s_cpu_timing_lwmutex, 1, NULL);
    return lock < 0 ? lock : sceKernelUnlockLwMutex(&s_cpu_timing_lwmutex, 1);
}
static int cpu_timing_call_vblank_start(void) { return sceDisplayWaitVblankStart(); }
static int cpu_timing_call_ge_enqueue(void) {
    if (s_cpu_timing_ge_count >= CPU_TIMING_GE_CALLS_PER_TRIAL) return -1;
    const int qid = sceGeListEnQueue(s_cpu_timing_ge_list,
                                     s_cpu_timing_ge_list, -1, NULL);
    if (qid < 0) return qid;
    s_cpu_timing_ge_qids[s_cpu_timing_ge_count++] = qid;
    return 0;
}
static int cpu_timing_call_ge_update_stall(void) {
    if (s_cpu_timing_ge_index >= s_cpu_timing_ge_count) return -1;
    const int qid = s_cpu_timing_ge_qids[s_cpu_timing_ge_index++];
    const uintptr_t stall_addr = (uintptr_t)s_cpu_timing_ge_list +
                                 s_cpu_timing_ge_list_bytes;
    return sceGeListUpdateStallAddr(qid, (void *)stall_addr);
}

static uint32_t cpu_timing_call_batch(CpuTimingCall fn, uint32_t calls, int *error) {
    const uint64_t start = (uint64_t)sceKernelGetSystemTimeWide();
    for (uint32_t i = 0; i < calls; i++) {
        const int rc = fn();
        if (rc < 0) { *error = rc; break; }
    }
    return (uint32_t)((uint64_t)sceKernelGetSystemTimeWide() - start);
}

static volatile CpuTimingDrawSyncJob s_cpu_timing_drawsync_job;

static int cpu_timing_drawsync_worker(SceSize args, void *argp) {
    (void)args;
    volatile CpuTimingDrawSyncJob *job = (volatile CpuTimingDrawSyncJob *)argp;
    for (uint32_t trial = 0; trial < CPU_TIMING_TRIALS; trial++) {
        const uint64_t start = (uint64_t)sceKernelGetSystemTimeWide();
        for (uint32_t i = 0; i < job->calls; i++) {
            const int rc = sceGeDrawSync(0);
            if (rc < 0) { job->error = rc; break; }
        }
        job->elapsed[trial] = (uint32_t)((uint64_t)sceKernelGetSystemTimeWide() - start);
        const uint64_t control_start = (uint64_t)sceKernelGetSystemTimeWide();
        for (uint32_t i = 0; i < job->calls; i++) (void)cpu_timing_noop_call();
        job->control[trial] =
            (uint32_t)((uint64_t)sceKernelGetSystemTimeWide() - control_start);
    }
    return 0;
}

static void cpu_timing_measure_drawsync(int emulated, uint32_t cpu_mhz) {
    memset((void *)&s_cpu_timing_drawsync_job, 0, sizeof(s_cpu_timing_drawsync_job));
    s_cpu_timing_drawsync_job.calls = CPU_TIMING_DRAW_SYNC_CALLS;
    const SceUID thread = sceKernelCreateThread("cpu-ge-sync", cpu_timing_drawsync_worker,
                                               32, 0x1000, THREAD_ATTR_USER, NULL);
    if (thread < 0) {
        uint32_t out[4] = {(uint32_t)thread, 0u, 0u, cpu_mhz};
        emit_record_extended(emulated, "PSP-CPU-TIMING-001", "syscall-ge-drawsync-done",
                             "FAIL", (uint32_t)thread, out, 4);
        return;
    }
    const int start_rc = sceKernelStartThread(thread, sizeof(s_cpu_timing_drawsync_job),
                                              (void *)&s_cpu_timing_drawsync_job);
    SceUInt timeout = 100000u;
    const int wait_rc = start_rc < 0 ? start_rc :
        sceKernelWaitThreadEnd(thread, &timeout);
    if (wait_rc < 0) {
        (void)sceKernelTerminateDeleteThread(thread);
        uint32_t out[4] = {(uint32_t)thread, (uint32_t)start_rc,
                           (uint32_t)wait_rc, cpu_mhz};
        emit_record_extended(emulated, "PSP-CPU-TIMING-001", "syscall-ge-drawsync-done",
                             "TIMEOUT", (uint32_t)wait_rc, out, 4);
        return;
    }
    (void)sceKernelDeleteThread(thread);
    uint32_t elapsed[CPU_TIMING_TRIALS], control[CPU_TIMING_TRIALS];
    for (uint32_t i = 0; i < CPU_TIMING_TRIALS; i++) {
        elapsed[i] = s_cpu_timing_drawsync_job.elapsed[i];
        control[i] = s_cpu_timing_drawsync_job.control[i];
    }
    cpu_timing_sort(elapsed);
    cpu_timing_sort(control);
    const uint32_t measured_us = elapsed[CPU_TIMING_TRIALS / 2u];
    const uint32_t control_us = control[CPU_TIMING_TRIALS / 2u];
    const uint32_t calls = CPU_TIMING_DRAW_SYNC_CALLS;
    const uint32_t delta_us = measured_us >= control_us ? measured_us - control_us : 0u;
    const uint32_t cycles_x100 =
        (uint32_t)(((uint64_t)delta_us * cpu_mhz * 100u + calls / 2u) / calls);
    uint32_t out[4] = {calls, measured_us, control_us, cpu_mhz};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "syscall-ge-drawsync-done",
                         s_cpu_timing_drawsync_job.error == 0 && measured_us >= control_us
                             ? "PASS" : "FAIL",
                         cycles_x100, out, 4);
}

static void cpu_timing_emit_syscall(int emulated, const char *case_id,
                                    CpuTimingCall fn, uint32_t calls,
                                    uint32_t syscalls_per_call, uint32_t cpu_mhz) {
    uint32_t elapsed[CPU_TIMING_TRIALS], baseline[CPU_TIMING_TRIALS];
    int error = 0;
    for (uint32_t trial = 0; trial < CPU_TIMING_TRIALS; trial++) {
        elapsed[trial] = cpu_timing_call_batch(fn, calls, &error);
        baseline[trial] = cpu_timing_call_batch(cpu_timing_noop_call, calls, &error);
    }
    cpu_timing_sort(elapsed);
    cpu_timing_sort(baseline);
    const uint32_t measured_us = elapsed[CPU_TIMING_TRIALS / 2u];
    const uint32_t control_us = baseline[CPU_TIMING_TRIALS / 2u];
    const uint32_t total_calls = calls * syscalls_per_call;
    const uint32_t delta_us = measured_us >= control_us ? measured_us - control_us : 0u;
    const uint32_t cycles_x100 = total_calls == 0u ? 0u :
        (uint32_t)(((uint64_t)delta_us * cpu_mhz * 100u + total_calls / 2u) / total_calls);
    uint32_t out[4] = {total_calls, measured_us, control_us, cpu_mhz};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", case_id,
                         error == 0 && measured_us >= control_us ? "PASS" : "FAIL",
                         cycles_x100, out, 4);
}

static void cpu_timing_emit_syscall_skip(int emulated, const char *case_id,
                                         uint32_t calls, uint32_t cpu_mhz) {
    uint32_t out[4] = {calls, 0u, 0u, cpu_mhz};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", case_id,
                         "SKIP", 0u, out, 4);
}

static int cpu_timing_ge_wait_idle(void) {
    const uint64_t start = (uint64_t)sceKernelGetSystemTimeWide();
    for (uint32_t polls = 0; polls < 10000u; polls++) {
        if (sceGeDrawSync(1) == 0) return 1;
        if ((uint64_t)sceKernelGetSystemTimeWide() - start >= 250000u) return 0;
        sceKernelDelayThread(100u);
    }
    return 0;
}

static void cpu_timing_emit_vblank(int emulated) {
    uint32_t elapsed[CPU_TIMING_TRIALS];
    int error = 0;
    for (uint32_t trial = 0; trial < CPU_TIMING_TRIALS; trial++)
        elapsed[trial] = cpu_timing_call_batch(cpu_timing_call_vblank_start, 1u, &error);
    cpu_timing_sort(elapsed);
    const uint32_t median_us = elapsed[CPU_TIMING_TRIALS / 2u];
    uint32_t out[4] = {CPU_TIMING_TRIALS, median_us, elapsed[0],
                       elapsed[CPU_TIMING_TRIALS - 1u]};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "syscall-vblank-start-latency",
                         error == 0 ? "PASS" : "FAIL", median_us, out, 4);
}

static void cpu_timing_emit_ge_cell(int emulated, const char *case_id,
                                    uint32_t *elapsed, uint32_t *baseline,
                                    uint32_t valid_trials, uint32_t cpu_mhz,
                                    int error, uint32_t enqueued,
                                    uint32_t update_attempts, int pass) {
    cpu_timing_sort(elapsed);
    cpu_timing_sort(baseline);
    const uint32_t measured_us = elapsed[CPU_TIMING_TRIALS / 2u];
    const uint32_t control_us = baseline[CPU_TIMING_TRIALS / 2u];
    const uint32_t delta_us = measured_us >= control_us ? measured_us - control_us : 0u;
    const uint32_t cycles_x100 = ((uint64_t)delta_us * cpu_mhz * 100u +
                                  CPU_TIMING_GE_CALLS_PER_TRIAL / 2u) /
                                 CPU_TIMING_GE_CALLS_PER_TRIAL;
    uint32_t out[7] = {CPU_TIMING_GE_CALLS_PER_TRIAL, measured_us, control_us, valid_trials,
                       (uint32_t)error, enqueued, update_attempts};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", case_id,
                         pass && measured_us >= control_us ? "PASS" : "FAIL",
                         cycles_x100, out, 7);
}

static void cpu_timing_emit_ge_skip(int emulated, const char *case_id) {
    uint32_t out[7] = {CPU_TIMING_GE_CALLS_PER_TRIAL, 0u, 0u, 0u, 0u, 0u, 0u};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", case_id,
                         "SKIP", 0u, out, 7);
}

/* The GE teardown row when sceGuInit was never called: same shape, all zero. */
static void cpu_timing_emit_gu_teardown_skip(int emulated) {
    const uint32_t out[GE_GU_TEARDOWN_OUTS] = {0u, 0u, 0u, 0u, 0u};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-gu-teardown",
                         "SKIP", 0u, out, GE_GU_TEARDOWN_OUTS);
}

static int cpu_timing_measure_ge_calls(int emulated, uint32_t cpu_mhz) {
    uint32_t enqueue[CPU_TIMING_TRIALS] = {0};
    uint32_t enqueue_base[CPU_TIMING_TRIALS] = {0};
    uint32_t update[CPU_TIMING_TRIALS] = {0};
    uint32_t update_base[CPU_TIMING_TRIALS] = {0};
    uint32_t valid = 0;
    int first_error = 0;
    uint32_t last_enqueued = 0;
    uint32_t last_update_attempts = 0;
    if (!cpu_timing_ge_wait_idle()) {
        cpu_timing_emit_ge_skip(emulated, "syscall-ge-list-enqueue");
        cpu_timing_emit_ge_skip(emulated, "syscall-ge-list-update-stall");
        cpu_timing_emit_gu_teardown_skip(emulated);
        return 0;
    }
    const int gu_rc = sceGuInit();
    int ready = gu_rc >= 0;
    if (ready) {
        /* This buffer is enqueued below, so libpspgu must emit a queued list to memory. */
        const int gu_start_rc = sceGuStart(GU_SEND, s_cpu_timing_ge_list);
        const int list_bytes = gu_start_rc >= 0 ? sceGuFinish() : gu_start_rc;
        ready = gu_start_rc >= 0 &&
            list_bytes >= (int)(2u * sizeof(s_cpu_timing_ge_list[0])) &&
            (uint32_t)list_bytes <= sizeof(s_cpu_timing_ge_list) &&
            ((uint32_t)list_bytes & (sizeof(s_cpu_timing_ge_list[0]) - 1u)) == 0u;
        if (ready) s_cpu_timing_ge_list_bytes = (uint32_t)list_bytes;
    }
    int pass = ready;
    if (ready) {
        sceKernelDcacheWritebackAll();
        probe_step(emulated, "cpu-timing", "ge-enqueue-stall-update");
    }
    for (uint32_t trial = 0; pass && trial < CPU_TIMING_TRIALS; trial++) {
        int error = 0;
        s_cpu_timing_ge_count = 0;
        s_cpu_timing_ge_index = 0;
        enqueue[trial] = cpu_timing_call_batch(cpu_timing_call_ge_enqueue,
                                               CPU_TIMING_GE_CALLS_PER_TRIAL, &error);
        enqueue_base[trial] = cpu_timing_call_batch(cpu_timing_noop_call,
                                                    CPU_TIMING_GE_CALLS_PER_TRIAL, &error);
        if (error < 0 && first_error == 0) first_error = error;
        last_enqueued = s_cpu_timing_ge_count;
        if (error == 0 && s_cpu_timing_ge_count == CPU_TIMING_GE_CALLS_PER_TRIAL) {
            update[trial] = cpu_timing_call_batch(cpu_timing_call_ge_update_stall,
                                                  CPU_TIMING_GE_CALLS_PER_TRIAL, &error);
            update_base[trial] = cpu_timing_call_batch(cpu_timing_noop_call,
                                                       CPU_TIMING_GE_CALLS_PER_TRIAL, &error);
            if (error < 0 && first_error == 0) first_error = error;
            last_update_attempts = s_cpu_timing_ge_index;
        } else {
            pass = 0;
        }
        if (error != 0 || !cpu_timing_ge_wait_idle()) pass = 0;
        if (pass) valid++;
        else break;
    }
    /* Reached on every path once sceGuInit has run: stop the GE if it is still
       busy, wait for it, then sceGuTerm, before any record is written. */
    probe_step(emulated, "cpu-timing", "gu-teardown");
    const GeGuTeardown gu_down = ge_gu_teardown(gu_rc, -1);
    const int idle = gu_down.idle;
    if (ready) {
        cpu_timing_emit_ge_cell(emulated, "syscall-ge-list-enqueue",
                                enqueue, enqueue_base, valid, cpu_mhz, first_error,
                                last_enqueued, last_update_attempts, pass && idle);
        cpu_timing_emit_ge_cell(emulated, "syscall-ge-list-update-stall",
                                update, update_base, valid, cpu_mhz, first_error,
                                last_enqueued, last_update_attempts, pass && idle);
    } else {
        cpu_timing_emit_ge_skip(emulated, "syscall-ge-list-enqueue");
        cpu_timing_emit_ge_skip(emulated, "syscall-ge-list-update-stall");
    }
    {
        uint32_t out[GE_GU_TEARDOWN_OUTS];
        ge_gu_teardown_outs(&gu_down, out);
        emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-gu-teardown",
                             idle ? "PASS" : "TIMEOUT", (uint32_t)gu_down.draw_state,
                             out, GE_GU_TEARDOWN_OUTS);
    }
    return pass && idle;
}

static void run_cpu_timing(int emulated) {
    static const uint32_t normal_blocks[] = {2048u, 8192u, 32768u};
    static const uint32_t miss_blocks[] = {32u, 128u, CPU_TIMING_MISS_MAX_BLOCKS};
    const int boot_cpu = scePowerGetCpuClockFrequencyInt();
    const int boot_bus = scePowerGetBusClockFrequencyInt();
    const int clock_rc = scePowerSetClockFrequency(333, 333, 166);
    const int cpu_mhz = scePowerGetCpuClockFrequencyInt();
    const int bus_mhz = scePowerGetBusClockFrequencyInt();
    uint32_t clock_out[4] = {(uint32_t)boot_cpu, (uint32_t)boot_bus,
                             (uint32_t)cpu_mhz, (uint32_t)bus_mhz};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-clock",
                         clock_rc >= 0 && cpu_mhz == 333 && bus_mhz == 166 ? "PASS" : "FAIL",
                         (uint32_t)clock_rc, clock_out, 4);
    if (clock_rc < 0 || cpu_mhz != 333 || bus_mhz != 166) goto restore_clock;

    __asm__ volatile("ctc1 $0,$31" ::: "memory");
    for (uint32_t i = 0; i < CPU_TIMING_MISS_BYTES / sizeof(uint32_t); i++)
        s_cpu_timing_memory[i] = 0x5a5aa5a5u ^ i;
    sceKernelDcacheWritebackAll();

    for (size_t i = 0; i < sizeof(normal_blocks) / sizeof(normal_blocks[0]); i++) {
        const uint32_t blocks = normal_blocks[i];
        cpu_timing_emit_instruction(emulated, "alu-addu", cpu_timing_alu,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "alu-shift", cpu_timing_shift,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "alu-logic", cpu_timing_logic,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "multu-mflo-pair", cpu_timing_mul_pair,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS * 2u,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "divu-mflo-pair", cpu_timing_div_pair,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS * 2u,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "load-cache-hit", cpu_timing_load_hit,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "store-cache-hit", cpu_timing_store_hit,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "branch-taken-delay", cpu_timing_branch_taken,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS * 2u,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "branch-not-taken-delay", cpu_timing_branch_not_taken,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS * 2u,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "branch-useful-delay", cpu_timing_branch_delay,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS * 2u,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "fpu-add-s", cpu_timing_fpu_add,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "fpu-mul-s", cpu_timing_fpu_mul,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "fpu-div-s", cpu_timing_fpu_div,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "vfpu-add-s", cpu_timing_vfpu_add,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
        cpu_timing_emit_instruction(emulated, "vfpu-mul-s", cpu_timing_vfpu_mul,
                                    cpu_timing_loop_empty, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 0);
    }
    for (size_t i = 0; i < sizeof(miss_blocks) / sizeof(miss_blocks[0]); i++) {
        const uint32_t blocks = miss_blocks[i];
        cpu_timing_emit_instruction(emulated, "load-cache-miss", cpu_timing_load_miss,
                                    cpu_timing_loop_walk, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 1);
        cpu_timing_emit_instruction(emulated, "store-cache-miss", cpu_timing_store_miss,
                                    cpu_timing_loop_walk, blocks, CPU_TIMING_OPS,
                                    (uint32_t)cpu_mhz, 1);
    }

    /* The display cell re-sets the displayed framebuffer to itself.  Standalone
       PSPLink (game context after a PSPLink reset) displays none, so there the
       probe displays its own 8888 buffer at the start of eDRAM, as a game
       would, and afterwards sets back what the PSP reported.  A set that
       changes the stride or format must wait for the next frame (IMMEDIATE
       returned 0x80000107 on 2026-10-27), so both changes use NEXTFRAME and
       wait for the vblank that latches them. */
    int frame_rc = sceDisplayGetFrameBuf(&s_cpu_timing_framebuffer,
                                         &s_cpu_timing_stride,
                                         &s_cpu_timing_format,
                                         PSP_DISPLAY_SETBUF_IMMEDIATE);
    const int frame_found_stride = s_cpu_timing_stride;
    const int frame_found_format = s_cpu_timing_format;
    int frame_owned = 0;
    int frame_restore = 0;
    if (frame_rc >= 0 && s_cpu_timing_framebuffer == NULL) {
        s_cpu_timing_framebuffer = sceGeEdramGetAddr();
        s_cpu_timing_stride = 512;
        s_cpu_timing_format = PSP_DISPLAY_PIXEL_FORMAT_8888;
        frame_rc = sceDisplaySetFrameBuf(s_cpu_timing_framebuffer, s_cpu_timing_stride,
                                         s_cpu_timing_format, PSP_DISPLAY_SETBUF_NEXTFRAME);
        frame_owned = frame_rc >= 0;
        if (frame_owned) (void)sceDisplayWaitVblankStart();
        else s_cpu_timing_framebuffer = NULL;
    }
    if (frame_rc >= 0 && s_cpu_timing_framebuffer != NULL) {
        cpu_timing_emit_syscall(emulated, "syscall-display-setframebuf",
                                cpu_timing_call_setframebuf, CPU_TIMING_SYSCALL_CALLS,
                                1u, (uint32_t)cpu_mhz);
    } else {
        cpu_timing_emit_syscall_skip(emulated, "syscall-display-setframebuf",
                                     CPU_TIMING_SYSCALL_CALLS, (uint32_t)cpu_mhz);
    }
    if (frame_owned) {
        frame_restore = sceDisplaySetFrameBuf(NULL, frame_found_stride, frame_found_format,
                                              PSP_DISPLAY_SETBUF_NEXTFRAME);
        if (frame_restore >= 0) (void)sceDisplayWaitVblankStart();
    }
    uint32_t frame_out[5] = {(uint32_t)frame_rc,
                             (uint32_t)(uintptr_t)s_cpu_timing_framebuffer,
                             (uint32_t)s_cpu_timing_stride,
                             (uint32_t)s_cpu_timing_format,
                             (uint32_t)frame_restore};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-framebuffer",
                         frame_rc >= 0 && s_cpu_timing_framebuffer != NULL ? "PASS" : "SKIP",
                         (uint32_t)frame_owned, frame_out, 5);
    cpu_timing_emit_syscall(emulated, "syscall-display-accumulated-hcount",
                            cpu_timing_call_hcount, CPU_TIMING_SYSCALL_CALLS,
                            1u, (uint32_t)cpu_mhz);
    probe_step(emulated, "cpu-timing", "ge-idle-precondition");
    const int ge_idle = cpu_timing_ge_wait_idle();
    uint32_t ge_idle_out[2] = {(uint32_t)ge_idle, (uint32_t)cpu_mhz};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-ge-idle",
                         ge_idle ? "PASS" : "SKIP", (uint32_t)ge_idle,
                         ge_idle_out, 2);
    if (ge_idle) {
        cpu_timing_measure_drawsync(emulated, (uint32_t)cpu_mhz);
        (void)cpu_timing_measure_ge_calls(emulated, (uint32_t)cpu_mhz);
    } else {
        uint32_t drawsync_out[4] = {CPU_TIMING_DRAW_SYNC_CALLS, 0u, 0u,
                                    (uint32_t)cpu_mhz};
        emit_record_extended(emulated, "PSP-CPU-TIMING-001", "syscall-ge-drawsync-done",
                             "SKIP", 0u, drawsync_out, 4);
        cpu_timing_emit_ge_skip(emulated, "syscall-ge-list-enqueue");
        cpu_timing_emit_ge_skip(emulated, "syscall-ge-list-update-stall");
        cpu_timing_emit_gu_teardown_skip(emulated);
    }
    cpu_timing_emit_vblank(emulated);

    s_cpu_timing_sema = sceKernelCreateSema("cpu-time", 0, 0, 1, NULL);
    const int lw_create = sceKernelCreateLwMutex(&s_cpu_timing_lwmutex,
                                                "cpu-time", 0, 0, NULL);
    uint32_t sync_out[2] = {(uint32_t)s_cpu_timing_sema, (uint32_t)lw_create};
    emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-sync-setup",
                         s_cpu_timing_sema >= 0 && lw_create >= 0 ? "PASS" : "FAIL",
                         (uint32_t)lw_create, sync_out, 2);
    if (s_cpu_timing_sema >= 0 && lw_create >= 0) {
        cpu_timing_emit_syscall(emulated, "syscall-sema-signal-wait",
                                cpu_timing_call_sema_pair, CPU_TIMING_SYSCALL_CALLS,
                                2u, (uint32_t)cpu_mhz);
        cpu_timing_emit_syscall(emulated, "syscall-lwmutex-lock-unlock",
                                cpu_timing_call_lwmutex_pair, CPU_TIMING_SYSCALL_CALLS,
                                2u, (uint32_t)cpu_mhz);
    } else {
        const uint32_t calls = CPU_TIMING_SYSCALL_CALLS * 2u;
        cpu_timing_emit_syscall_skip(emulated, "syscall-sema-signal-wait",
                                     calls, (uint32_t)cpu_mhz);
        cpu_timing_emit_syscall_skip(emulated, "syscall-lwmutex-lock-unlock",
                                     calls, (uint32_t)cpu_mhz);
    }
    if (lw_create >= 0) (void)sceKernelDeleteLwMutex(&s_cpu_timing_lwmutex);

restore_clock:
    {
        const int restore_rc = scePowerSetClockFrequency(boot_cpu, boot_cpu, boot_bus);
        uint32_t done_out[3] = {(uint32_t)restore_rc,
                                (uint32_t)scePowerGetCpuClockFrequencyInt(),
                                (uint32_t)scePowerGetBusClockFrequencyInt()};
        emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-done",
                             restore_rc >= 0 ? "PASS" : "FAIL",
                             (uint32_t)restore_rc, done_out, 3);
        uint32_t complete_out[1] = {CPU_TIMING_RESULT_COUNT};
        emit_record_extended(emulated, "PSP-CPU-TIMING-001", "cpu-timing-complete",
                             "PASS", CPU_TIMING_RESULT_COUNT, complete_out, 1);
    }
}
#undef CPU_TIMING_REPEAT_8
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
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR || PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_CC
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
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS
    run_vfpu_transcendental_corpus(emulated);
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
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_LLSC_LINK
    run_llsc_link(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_CC
    run_fpu_cc(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_FONT
    run_registry_font(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_FILES
    run_font_files(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_IO
    run_umd_io(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_UMD_RAW_READ
    run_umdraw(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_LIST
    run_font_list(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_VRAM_MIRROR
    run_vram_mirror(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_LATE_PRX_TARGET
    run_late_prx_target(emulated); /* parks; never returns */
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AT3_DECODE
    run_at3_decode(emulated); /* ends in probe_teardown, which parks until modstop */
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FAKE_SUCCESS
    run_fake_success(emulated); /* ends in probe_teardown, which parks until modstop */
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FONT_USERPGF
    run_font_userpgf(emulated); /* probe_teardown below parks until modstop */
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_TIMING
    run_ge_timing(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING
    run_cpu_timing(emulated);
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
