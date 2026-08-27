/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Generic runtime title configuration.
 *
 * The runtime owns every PSP semantic; a title configuration owns only *where* --
 * and *whether* -- a title-specific address or role binding applies. Nothing in this
 * interface can redefine scheduler, kernel, or GE behavior: a binding either names a
 * guest address the generic code already knows what to do with, or it is absent and
 * that code path does not run at all.
 *
 * The values reach the build from validated title configuration through
 * tools/title_runtime_config.py, which emits a build-local header consumed only by
 * title_config.c. A build with no title configuration has every optional binding
 * disabled, and the accessors below then report "not configured" for every guest
 * address -- including an address that a previous release happened to hardcode.
 */
#ifndef SR_TITLE_CONFIG_H
#define SR_TITLE_CONFIG_H

#include <stdint.h>

/* Validity bits. A bit is set only when the build's title configuration supplied
 * that binding. Paired bindings share one bit because a half-configured pair has
 * no meaning; the manifest validator rejects that shape before it reaches here. */
#define SR_TITLE_CFG_FALLBACK_ENTRY   0x1u
#define SR_TITLE_CFG_WORKER_ENTRY     0x2u
#define SR_TITLE_CFG_LAUNCHER_ENTRY   0x4u
#define SR_TITLE_CFG_VBLANK_COUNTERS  0x8u
/* Collection bits. Set only when the configuration supplied a NON-EMPTY collection;
 * the manifest validator rejects an empty one, so a set bit always means >= 1 entry. */
#define SR_TITLE_CFG_DISPATCH_ALIASES      0x10u
#define SR_TITLE_CFG_CALLBACK_TERMINATORS  0x20u
/* Compatibility-profile bits. Each names a typed, title-qualified capability that
 * is either fully configured or absent. See tools/title_manifest.py for shape. */
#define SR_TITLE_CFG_DISPLAY_BRINGUP       0x40u
#define SR_TITLE_CFG_RUNTIME_SYNC          0x80u
#define SR_TITLE_CFG_LIBFONT_READY         0x100u
#define SR_TITLE_CFG_FRAME_LATCH           0x200u

/* One dispatch alias: a computed call to `from` must enter the body registered at `to`.
 * This does not invent behavior -- the runtime still executes an ordinary registered
 * function -- it only covers a registration gap, such as a tail call that lands past a
 * callee's prologue at an address codegen never registered separately. */
typedef struct SrTitleDispatchAlias {
    uint32_t from;
    uint32_t to;
} SrTitleDispatchAlias;

/* One callback terminator: at this exact call site, `sentinel` as a dispatch target
 * means the guest's callback walk is COMPLETE. Without it the sentinel is an ordinary
 * permissive miss, which returns "continue" and loops a circular walker forever.
 *
 * `has_pc`/`has_ra` distinguish "constrain this to a value" from "do not constrain it".
 * The validator requires at least one constraint, so a terminator can never match a
 * sentinel program-wide. An absent constraint is never compared against a placeholder. */
typedef struct SrTitleCallbackTerminator {
    uint32_t sentinel;
    unsigned has_pc;
    uint32_t pc;
    unsigned has_ra;
    uint32_t ra;
} SrTitleCallbackTerminator;

typedef struct SrTitleDisplayBringup {
    uint32_t malloc_entry;
    uint32_t vblank_device_init_entry;
    uint32_t render_context_init_entry;
    uint32_t render_context_magic_addr;
    uint32_t render_table_ready_flag_addr;
    uint32_t render_context_word_addr;
} SrTitleDisplayBringup;

typedef struct SrTitleRuntimeSyncWrapper {
    uint32_t mode;
    uint32_t enter;
    uint32_t leave;
} SrTitleRuntimeSyncWrapper;

typedef struct SrTitleRuntimeConfig {
    unsigned    valid;                        /* OR of the SR_TITLE_CFG_* bits above */
    uint32_t    fallback_entry;               /* module-start fallback when the image entry is uncompiled */
    uint32_t    worker_thread_entry;          /* thread entry that carries the title's worker role */
    uint32_t    launcher_thread_entry;        /* thread entry that carries the title's launcher role */
    uint32_t    vblank_frame_counter_addr;    /* guest word incremented once per delivered VBLANK */
    uint32_t    vblank_vsync_counter_addr;    /* guest word incremented once per delivered VBLANK */
    uint32_t    libfont_ready_flag_addr;      /* guest word forced to 1 when libfont.prx loads */
    uint32_t    frame_ready_latch_addr;       /* guest counter that gates frame presentation */
    SrTitleDisplayBringup display_bringup;    /* valid only when DISPLAY_BRINGUP bit set */
    uint32_t    runtime_sync_config_base;     /* base of HST sync config block */
    uint32_t    runtime_sync_sema_name_ptr;   /* name ptr handed to sceKernelCreateSema */
    const char *source_id;                    /* validated title id, or "none" */

    /* Typed collections. Both are empty (count 0) in a generic build. The pointers are
     * never NULL so a caller cannot dereference one by mistake; count is the authority. */
    const SrTitleDispatchAlias       *dispatch_aliases;
    unsigned                          dispatch_alias_count;
    const SrTitleCallbackTerminator  *callback_terminators;
    unsigned                          callback_terminator_count;
    const SrTitleRuntimeSyncWrapper  *runtime_sync_wrappers;
    unsigned                          runtime_sync_wrapper_count;
} SrTitleRuntimeConfig;

/* The build's configuration. Never NULL; a generic build reports valid == 0. */
const SrTitleRuntimeConfig *sr_title_config(void);

/* Configured fallback entry, or 0 when the build has none. 0 is not a usable guest
 * entry here, and the manifest validator rejects a configured zero, so callers can
 * treat 0 as "unconfigured" without a second predicate. */
uint32_t sr_title_config_fallback_entry(void);

/* Role predicates. Both answer 0 for every entry -- including entry 0 -- when the
 * corresponding binding is unconfigured, so an unconfigured build can never match a
 * role by accident. */
int sr_title_config_is_worker_entry(uint32_t entry);
int sr_title_config_is_launcher_entry(uint32_t entry);

/* Returns 1 and fills both addresses when the paired VBLANK counters are configured,
 * 0 otherwise (leaving the outputs untouched). */
int sr_title_config_vblank_counters(uint32_t *frame_addr, uint32_t *vsync_addr);

/* Dispatch alias lookup. Returns 1 and writes the aliased body's address to *to_addr
 * when `from` is a configured alias source, 0 otherwise (leaving *to_addr untouched).
 * An unconfigured build answers 0 for every address, so a generic runtime inherits no
 * redirect at all -- an address that a previous release happened to redirect included.
 * Matching is exact: a neighbouring address is not an alias of anything. */
int sr_title_config_dispatch_alias(uint32_t from, uint32_t *to_addr);

/* Callback-terminator match. Returns 1 only when some configured terminator names this
 * sentinel AND every constraint it declares (pc, ra, or both) holds at this call site.
 * An unconfigured build answers 0 for every triple, so the same sentinel at the same
 * site follows ordinary generic dispatch behavior instead. */
int sr_title_config_is_callback_terminator(uint32_t sentinel, uint32_t pc, uint32_t ra);

/* Display bringup. Returns 1 and fills *out when the title configures the
 * display-driver bringup replay, 0 otherwise. An unconfigured build answers 0
 * for every caller, so generic sceDisplaySetMode performs no guest calls. */
int sr_title_config_display_bringup(SrTitleDisplayBringup *out);

/* Runtime sync. Returns 1 and fills base/name/wrappers when configured, 0
 * otherwise. Wrappers are mode-keyed pairs; the mode that selects a pair is
 * part of the meaning and is not flattened. */
int sr_title_config_runtime_sync(uint32_t *config_base, uint32_t *sema_name_ptr,
                                 const SrTitleRuntimeSyncWrapper **wrappers,
                                 unsigned *count);

/* Convenience: find the wrapper pair for a specific mode. Returns 1 on hit. */
int sr_title_config_runtime_sync_wrapper_for_mode(uint32_t mode,
                                                  uint32_t *enter, uint32_t *leave);

/* Single-address compat flags. Returns 1 when configured. */
int sr_title_config_libfont_ready_flag_addr(uint32_t *out);
int sr_title_config_frame_latch_addr(uint32_t *out);

#endif /* SR_TITLE_CONFIG_H */
