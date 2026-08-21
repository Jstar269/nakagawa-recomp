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

typedef struct SrTitleRuntimeConfig {
    unsigned    valid;                        /* OR of the SR_TITLE_CFG_* bits above */
    uint32_t    fallback_entry;               /* module-start fallback when the image entry is uncompiled */
    uint32_t    worker_thread_entry;          /* thread entry that carries the title's worker role */
    uint32_t    launcher_thread_entry;        /* thread entry that carries the title's launcher role */
    uint32_t    vblank_frame_counter_addr;    /* guest word incremented once per delivered VBLANK */
    uint32_t    vblank_vsync_counter_addr;    /* guest word incremented once per delivered VBLANK */
    const char *source_id;                    /* validated title id, or "none" */
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

#endif /* SR_TITLE_CONFIG_H */
