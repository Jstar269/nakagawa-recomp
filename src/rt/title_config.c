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

#if SR_TITLE_CONFIG_SCHEMA_VERSION != 1
#error "generated title runtime configuration uses an unsupported schema version"
#endif

static const SrTitleRuntimeConfig s_config = {
    SR_TITLE_CONFIG_VALID,
    SR_TITLE_CONFIG_FALLBACK_ENTRY,
    SR_TITLE_CONFIG_WORKER_THREAD_ENTRY,
    SR_TITLE_CONFIG_LAUNCHER_THREAD_ENTRY,
    SR_TITLE_CONFIG_VBLANK_FRAME_COUNTER_ADDR,
    SR_TITLE_CONFIG_VBLANK_VSYNC_COUNTER_ADDR,
    SR_TITLE_CONFIG_SOURCE_ID,
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
