/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * The single translation unit that knows a title configuration exists.
 *
 * It binds the build-local artifact emitted by tools/title_runtime_config.py to the
 * generic SrTitleRuntimeConfig interface. Every other runtime source consumes the
 * accessors in title_config.h and never sees a title address, a macro name, or a
 * conditional compilation branch.
 */

#include "title_config.h"

/* Build-local generated artifact (build/<game>/sr_title_config.h). The build always
 * generates it: with no title configuration it defines the generic all-disabled
 * configuration, so this include is unconditional and a missing artifact is a build
 * failure rather than a silent fallback to some other title's behavior. */
#include "sr_title_config.h"

#if SR_TITLE_CONFIG_SCHEMA_VERSION != 3
#error "generated title runtime configuration uses an unsupported schema version"
#endif

/* The generated artifact states the collections as X-macro lists; this file owns the C
 * types they expand into. A generic build expands both lists to nothing, so the arrays
 * below hold only their unused placeholder element and both counts are 0. */
#define SR_TITLE_CFG_ALIAS(from_addr, to_addr) { (from_addr), (to_addr) },
/* Deliberately UNSIZED: the size then comes from the list itself, which is what makes
 * the assertion below a proof rather than a restatement. Sizing the array by the count
 * would zero-pad a short list instead of diagnosing it, and a zero-filled alias entry is
 * a live entry the lookup would compare against. */
static const SrTitleDispatchAlias s_dispatch_aliases[] = {
    SR_TITLE_CONFIG_DISPATCH_ALIAS_LIST
    { 0u, 0u }  /* placeholder: C has no zero-length array, and it is never read */
};
#undef SR_TITLE_CFG_ALIAS
_Static_assert(sizeof s_dispatch_aliases / sizeof s_dispatch_aliases[0]
                   == SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT + 1u,
               "generated dispatch-alias list does not match its declared count");

#define SR_TITLE_CFG_TERMINATOR(s, hp, p, hr, r) { (s), (hp), (p), (hr), (r) },
/* Unsized for the same reason, and here the stakes are higher: a zero-filled terminator
 * entry reads as {sentinel 0, no pc constraint, no ra constraint}, which is precisely
 * the program-wide match the manifest validator refuses to accept. The assertion makes a
 * count/list disagreement a compile error, so that entry can never become reachable. */
static const SrTitleCallbackTerminator s_callback_terminators[] = {
    SR_TITLE_CONFIG_CALLBACK_TERMINATOR_LIST
    { 0u, 0u, 0u, 0u, 0u }  /* placeholder: never read; the count is the authority */
};
#undef SR_TITLE_CFG_TERMINATOR
_Static_assert(sizeof s_callback_terminators / sizeof s_callback_terminators[0]
                   == SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT + 1u,
               "generated callback-terminator list does not match its declared count");

#define SR_TITLE_CFG_RUNTIME_SYNC_WRAPPER(m, e, l) { (m), (e), (l) },
static const SrTitleRuntimeSyncWrapper s_runtime_sync_wrappers[] = {
    SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_LIST
    { 0u, 0u, 0u }
};
#undef SR_TITLE_CFG_RUNTIME_SYNC_WRAPPER
_Static_assert(sizeof s_runtime_sync_wrappers / sizeof s_runtime_sync_wrappers[0]
                   == SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_COUNT + 1u,
               "generated runtime-sync wrapper list does not match its declared count");

static const SrTitleRuntimeConfig s_config = {
    SR_TITLE_CONFIG_VALID,
    SR_TITLE_CONFIG_FALLBACK_ENTRY,
    SR_TITLE_CONFIG_WORKER_THREAD_ENTRY,
    SR_TITLE_CONFIG_LAUNCHER_THREAD_ENTRY,
    SR_TITLE_CONFIG_VBLANK_FRAME_COUNTER_ADDR,
    SR_TITLE_CONFIG_VBLANK_VSYNC_COUNTER_ADDR,
    SR_TITLE_CONFIG_LIBFONT_READY_FLAG_ADDR,
    SR_TITLE_CONFIG_FRAME_READY_LATCH_ADDR,
    {
        SR_TITLE_CONFIG_DISPLAY_BRINGUP_MALLOC_ENTRY,
        SR_TITLE_CONFIG_DISPLAY_BRINGUP_VBLANK_DEVICE_INIT_ENTRY,
        SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_INIT_ENTRY,
        SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_MAGIC_ADDR,
        SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_TABLE_READY_FLAG_ADDR,
        SR_TITLE_CONFIG_DISPLAY_BRINGUP_RENDER_CONTEXT_WORD_ADDR
    },
    SR_TITLE_CONFIG_RUNTIME_SYNC_CONFIG_BASE,
    SR_TITLE_CONFIG_RUNTIME_SYNC_SEMA_NAME_PTR,
    SR_TITLE_CONFIG_SOURCE_ID,
    s_dispatch_aliases,
    (unsigned)SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT,
    s_callback_terminators,
    (unsigned)SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT,
    s_runtime_sync_wrappers,
    (unsigned)SR_TITLE_CONFIG_RUNTIME_SYNC_WRAPPER_COUNT,
};

const SrTitleRuntimeConfig *sr_title_config(void) { return &s_config; }

uint32_t sr_title_config_fallback_entry(void) {
    return (s_config.valid & SR_TITLE_CFG_FALLBACK_ENTRY) ? s_config.fallback_entry : 0u;
}

int sr_title_config_is_worker_entry(uint32_t entry) {
    return (s_config.valid & SR_TITLE_CFG_WORKER_ENTRY) != 0u &&
           entry == s_config.worker_thread_entry;
}

int sr_title_config_is_launcher_entry(uint32_t entry) {
    return (s_config.valid & SR_TITLE_CFG_LAUNCHER_ENTRY) != 0u &&
           entry == s_config.launcher_thread_entry;
}

int sr_title_config_vblank_counters(uint32_t *frame_addr, uint32_t *vsync_addr) {
    if (!(s_config.valid & SR_TITLE_CFG_VBLANK_COUNTERS)) return 0;
    if (frame_addr) *frame_addr = s_config.vblank_frame_counter_addr;
    if (vsync_addr) *vsync_addr = s_config.vblank_vsync_counter_addr;
    return 1;
}

int sr_title_config_dispatch_alias(uint32_t from, uint32_t *to_addr) {
    /* The validity bit is checked first so an unconfigured build returns without
     * examining the placeholder element at all. */
    if (!(s_config.valid & SR_TITLE_CFG_DISPATCH_ALIASES)) return 0;
    for (unsigned i = 0; i < s_config.dispatch_alias_count; i++) {
        /* Exact match only: the validator guarantees `from` is unique across the
         * collection, so the first hit is the only hit. */
        if (s_config.dispatch_aliases[i].from != from) continue;
        if (to_addr) *to_addr = s_config.dispatch_aliases[i].to;
        return 1;
    }
    return 0;
}

int sr_title_config_is_callback_terminator(uint32_t sentinel, uint32_t pc, uint32_t ra) {
    if (!(s_config.valid & SR_TITLE_CFG_CALLBACK_TERMINATORS)) return 0;
    for (unsigned i = 0; i < s_config.callback_terminator_count; i++) {
        const SrTitleCallbackTerminator *t = &s_config.callback_terminators[i];
        if (t->sentinel != sentinel) continue;
        /* A constraint that was not configured is not compared -- never compared
         * against 0, which would silently narrow the site the title actually named. */
        if (t->has_pc && t->pc != pc) continue;
        if (t->has_ra && t->ra != ra) continue;
        return 1;
    }
    return 0;
}

int sr_title_config_display_bringup(SrTitleDisplayBringup *out) {
    if (!(s_config.valid & SR_TITLE_CFG_DISPLAY_BRINGUP)) return 0;
    if (out) *out = s_config.display_bringup;
    return 1;
}

int sr_title_config_runtime_sync(uint32_t *config_base, uint32_t *sema_name_ptr,
                                 const SrTitleRuntimeSyncWrapper **wrappers,
                                 unsigned *count) {
    if (!(s_config.valid & SR_TITLE_CFG_RUNTIME_SYNC)) return 0;
    if (config_base) *config_base = s_config.runtime_sync_config_base;
    if (sema_name_ptr) *sema_name_ptr = s_config.runtime_sync_sema_name_ptr;
    if (wrappers) *wrappers = s_config.runtime_sync_wrappers;
    if (count) *count = s_config.runtime_sync_wrapper_count;
    return 1;
}

int sr_title_config_runtime_sync_wrapper_for_mode(uint32_t mode,
                                                  uint32_t *enter, uint32_t *leave) {
    if (!(s_config.valid & SR_TITLE_CFG_RUNTIME_SYNC)) return 0;
    for (unsigned i = 0; i < s_config.runtime_sync_wrapper_count; i++) {
        if (s_config.runtime_sync_wrappers[i].mode != mode) continue;
        if (enter) *enter = s_config.runtime_sync_wrappers[i].enter;
        if (leave) *leave = s_config.runtime_sync_wrappers[i].leave;
        return 1;
    }
    return 0;
}

int sr_title_config_libfont_ready_flag_addr(uint32_t *out) {
    if (!(s_config.valid & SR_TITLE_CFG_LIBFONT_READY)) return 0;
    if (out) *out = s_config.libfont_ready_flag_addr;
    return 1;
}

int sr_title_config_frame_latch_addr(uint32_t *out) {
    if (!(s_config.valid & SR_TITLE_CFG_FRAME_LATCH)) return 0;
    if (out) *out = s_config.frame_ready_latch_addr;
    return 1;
}
