// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Code a game installs at run time (#1933): capture, translate, bind by content.
 *
 * A synthetic guest copies a small routine (a function that calls a leaf) into main RAM that
 * no executable span owns, as a game unpacks code into its BSS, announces it with
 * instruction-cache maintenance (sr_module_code_sync_overlays) and calls it.
 *
 *   code_capture_selftest_capture.exe <folder>
 *       No translation covers the routine. The call stops as not-executable with v0 and pc
 *       untouched, and with SR_CODE_CAPTURE=<folder> the runtime writes one capture of the
 *       unowned RAM around the target, which ends where an executable span begins. Without
 *       SR_CODE_CAPTURE nothing is written.
 *
 *   tools/code_capture.py translate --out <unit.c> <folder>
 *
 *   code_capture_selftest_bound.exe <folder>   (built with the unit)
 *       The translation is bound while memory holds the routine, whatever the data around it
 *       holds, and takes the call. Once one word of its code differs, it is unbound: the call
 *       stops again and writes a capture of the new bytes. With the code restored it is bound
 *       again.
 *
 * Standalone host executable, no game inputs. The harness #includes recomp.c and drives the
 * production dispatch_call_try(); setup is test-specific, so this is white-box evidence.
 * The expected stops print DISPATCH_MISS_NEW / INTERP_REJECT / CODE_CAPTURE lines on stderr;
 * the exit code is the result.
 */

#include "sr_coro.h"
#include "recomp.c"   /* white-box: the real dispatch core and its capture writer */

#include <stdlib.h>
#include <string.h>

/* No GE renderer is linked by this dispatch-only harness. */
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

/* ---- stubs for runtime symbols recomp.c references -------------------------------- */

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}
CpuState *s_cpu = NULL;
int sr_sched_on = 0;
atomic_int_least32_t sr_timeslice;
uint32_t sched_current_uid(void) { return 0u; }
void sched_exit_current(int32_t status) { (void)status; }
void sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void sched_set_current_join_target(uint32_t uid) { (void)uid; }
void sched_clear_current_join_target(void) {}
int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t sr_hle_resolve_late_import(uint32_t target) { (void)target; return 0u; }
uint32_t sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; return 0u; }
void sr_yield(CpuState *s) { (void)s; }
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* ---- the synthetic guest ------------------------------------------------------------ */

static int g_failures = 0;
#define CHECK(cond, ...) do {                                            \
    if (!(cond)) {                                                       \
        g_failures++;                                                    \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);             \
        fprintf(stderr, __VA_ARGS__);                                    \
        fprintf(stderr, "\n");                                           \
    }                                                                    \
} while (0)

#define CAPTURE_TARGET 0x09c1b780u          /* the routine's entry */
#define CAPTURE_LEAF   (CAPTURE_TARGET + 0x20u)
#define CAPTURE_DATA   (CAPTURE_TARGET + 0x100u) /* data the game keeps beside the code */
#define MAIN_SPAN_LO   (CAPTURE_TARGET + 0x400u) /* an executable span above the routine */
#define MAIN_SPAN_HI   (CAPTURE_TARGET + 0x500u)
#define CALLER_PC      0x08804010u
#define RESUME_PC      0x08804018u
#define LEAF_ADDIU     0x24820030u               /* addiu v0, a0, 0x30 */
#define GUEST_SP       0x09f00000u

static void install_routine(uint32_t leaf_word, uint32_t data_word) {
    static const uint32_t entry[] = {
        0x27bdfff0u,                                       /* addiu sp, sp, -16 */
        0xafbf0000u,                                       /* sw    ra, 0(sp) */
        0x0c000000u | ((CAPTURE_LEAF >> 2) & 0x03ffffffu), /* jal   leaf */
        0x24040005u,                                       /* addiu a0, zero, 5 */
        0x8fbf0000u,                                       /* lw    ra, 0(sp) */
        0x03e00008u,                                       /* jr    ra */
        0x27bd0010u,                                       /* addiu sp, sp, 16 */
    };
    for (unsigned i = 0; i < sizeof entry / sizeof entry[0]; i++)
        MEM_W32(CAPTURE_TARGET + 4u * i, entry[i]);
    MEM_W32(CAPTURE_LEAF, leaf_word);
    MEM_W32(CAPTURE_LEAF + 4u, 0x03e00008u);               /* jr    ra */
    MEM_W32(CAPTURE_LEAF + 8u, 0x00000000u);               /* nop */
    /* A prologue-shaped data word: the capture holds it, the translation must not use it. */
    MEM_W32(CAPTURE_DATA, data_word);
    MEM_W32(CAPTURE_DATA + 4u, 0x41544144u);
}

typedef struct {
    int result;
    uint32_t v0, pc, sp;
} Call;

static Call call_routine(void) {
    CpuState s;
    memset(&s, 0, sizeof s);
    s.pc = CALLER_PC;
    s.r[29] = GUEST_SP;
    s.r[31] = RESUME_PC;
    s.r[2] = 0xdeadbeefu;   /* poison: only the routine may write v0 */
    s_code_capture_last[0] = '\0';
    Call out;
    out.result = dispatch_call_try(&s, CAPTURE_TARGET, RESUME_PC);
    out.v0 = s.r[2];
    out.pc = s.pc;
    out.sp = s.r[29];
    return out;
}

static void set_capture_folder(const char *folder) {
#ifdef _WIN32
    _putenv_s("SR_CODE_CAPTURE", folder ? folder : "");
#else
    if (folder) setenv("SR_CODE_CAPTURE", folder, 1);
    else unsetenv("SR_CODE_CAPTURE");
#endif
}

static void guest_setup(void) {
    sr_mem_init();
    atomic_store(&sr_timeslice, 0);
    sr_exec_span_reset();
    CHECK(sr_exec_span_register(MAIN_SPAN_LO, MAIN_SPAN_HI), "main executable span refused");
}

#ifndef CODE_CAPTURE_TRANSLATED

static int read_capture(const char *folder, const char *name, unsigned char *header,
                        size_t header_size, long *file_size) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    size_t got = fread(header, 1u, header_size, in);
    fseek(in, 0, SEEK_END);
    *file_size = ftell(in);
    fclose(in);
    return got == header_size;
}

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int run_capture(const char *folder) {
    guest_setup();
    install_routine(LEAF_ADDIU, 0x27bdffe0u);
    CHECK(sr_module_code_sync_overlays(0u, 0xffffffffu) == 0u, "nothing is translated yet");

    set_capture_folder(NULL);
    Call quiet = call_routine();
    CHECK(quiet.result == SR_GUEST_INTERP_NOT_EXECUTABLE && quiet.v0 == 0xdeadbeefu &&
              quiet.pc == CALLER_PC,
          "untranslated installed code stops as not-executable (result %d v0 0x%08x pc 0x%08x)",
          quiet.result, quiet.v0, quiet.pc);
    CHECK(s_code_capture_last[0] == '\0', "no capture without SR_CODE_CAPTURE");

    set_capture_folder(folder);
    Call stop = call_routine();
    CHECK(stop.result == SR_GUEST_INTERP_NOT_EXECUTABLE && stop.v0 == 0xdeadbeefu &&
              stop.pc == CALLER_PC,
          "with SR_CODE_CAPTURE the call still stops (result %d v0 0x%08x pc 0x%08x)",
          stop.result, stop.v0, stop.pc);
    unsigned char header[32];
    long size = 0;
    CHECK(s_code_capture_last[0] != '\0' &&
              read_capture(folder, s_code_capture_last, header, sizeof header, &size),
          "the stop wrote a capture (%s)", s_code_capture_last);
    if (size > 0) {
        uint32_t load = get32(header + 8), bytes = get32(header + 12);
        CHECK(memcmp(header, "NKCODE01", 8u) == 0, "capture magic");
        CHECK(get32(header + 16) == CAPTURE_TARGET && get32(header + 24) == RESUME_PC,
              "capture names the target and $ra");
        CHECK(load + bytes == MAIN_SPAN_LO, "the window ends where executable code begins "
              "(0x%08x)", load + bytes);
        CHECK(load == CAPTURE_TARGET - SR_CODE_CAPTURE_REACH,
              "the window reaches SR_CODE_CAPTURE_REACH below the target (0x%08x)", load);
        CHECK(size == 32 + (long)bytes, "the file is the header and the window");
    }
    char first[sizeof s_code_capture_last];
    memcpy(first, s_code_capture_last, sizeof first);
    Call again = call_routine();
    CHECK(again.result < 0 && strcmp(s_code_capture_last, first) == 0,
          "the same bytes make the same capture");
    return g_failures;
}

#else  /* CODE_CAPTURE_TRANSLATED: linked with tools/code_capture.py's unit */

void sr_register_code_captures(void);

static int run_bound(const char *folder) {
    guest_setup();
    set_capture_folder(folder);
    sr_register_code_captures();
    CHECK(sr_module_code_count() >= 1u, "the unit registers the translation");

    /* The data beside the code differs from the capture: it is not the translation's
     * identity, so the translation still binds. */
    install_routine(LEAF_ADDIU, 0x13572468u);
    CHECK(sr_module_code_sync_overlays(0u, 0xffffffffu) == 1u,
          "instruction-cache maintenance binds the translation of the installed code");
    Call bound = call_routine();
    CHECK(bound.result >= 0 && bound.v0 == 0x35u && bound.sp == GUEST_SP,
          "the translation takes the call and returns (result %d v0 0x%08x sp 0x%08x)",
          bound.result, bound.v0, bound.sp);
    CHECK(s_code_capture_last[0] == '\0', "a bound call writes no capture");

    install_routine(LEAF_ADDIU + 1u, 0x13572468u);   /* addiu v0, a0, 0x31 */
    CHECK(sr_module_code_sync_overlays(0u, 0xffffffffu) == 1u,
          "code whose bytes differ unbinds the translation");
    Call differ = call_routine();
    CHECK(differ.result == SR_GUEST_INTERP_NOT_EXECUTABLE && differ.v0 == 0xdeadbeefu,
          "a call into differing bytes is never bound: it stops (result %d v0 0x%08x)",
          differ.result, differ.v0);
    CHECK(s_code_capture_last[0] != '\0', "the differing bytes make a capture of their own");

    install_routine(LEAF_ADDIU, 0x13572468u);
    CHECK(sr_module_code_sync_overlays(0u, 0xffffffffu) == 1u,
          "the restored code binds again");
    Call rebound = call_routine();
    CHECK(rebound.result >= 0 && rebound.v0 == 0x35u, "the rebound translation takes the call");
    return g_failures;
}

#endif

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <capture folder>\n", argv[0]);
        return 2;
    }
#ifndef CODE_CAPTURE_TRANSLATED
    int failures = run_capture(argv[1]);
    const char *phase = "capture";
#else
    int failures = run_bound(argv[1]);
    const char *phase = "bound";
#endif
    fprintf(stderr, "code-capture-selftest (%s): %s (%d failure(s))\n", phase,
            failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
