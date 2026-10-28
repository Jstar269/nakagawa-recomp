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
/* 0x8u retired (#643): VBLANK counter binding removed */
/* Collection bits. Set only when the configuration supplied a NON-EMPTY collection;
 * the manifest validator rejects an empty one, so a set bit always means >= 1 entry. */
#define SR_TITLE_CFG_DISPATCH_ALIASES      0x10u
#define SR_TITLE_CFG_CALLBACK_TERMINATORS  0x20u
/* 0x40u, 0x80u and 0x200u retired (#363): display bring-up, runtime sync and frame latch
 * bindings removed */
#define SR_TITLE_CFG_EXPECTED_DATA_FILE_COUNT 0x400u

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


typedef struct SrTitleRuntimeConfig {
    unsigned    valid;                        /* OR of the SR_TITLE_CFG_* bits above */
    uint32_t    fallback_entry;               /* module-start fallback when the image entry is uncompiled */
    uint32_t    worker_thread_entry;          /* thread entry that carries the title's worker role */
    uint32_t    launcher_thread_entry;        /* thread entry that carries the title's launcher role */
    uint32_t    expected_data_file_count;     /* 0 = no census; else exact file count under data_root */
    const char *source_id;                    /* validated title id, or "none" */

    /* Typed collections. Both are empty (count 0) in a generic build. The pointers are
     * never NULL so a caller cannot dereference one by mistake; count is the authority. */
    const SrTitleDispatchAlias       *dispatch_aliases;
    unsigned                          dispatch_alias_count;
    const SrTitleCallbackTerminator  *callback_terminators;
    unsigned                          callback_terminator_count;
} SrTitleRuntimeConfig;

/* The build's configuration. Never NULL; a generic build reports valid == 0. */
const SrTitleRuntimeConfig *sr_title_config(void);

/* HST-profile diagnostic opt-in. Returns 1 only when the validated build profile
 * permits the retained title-scoped diagnostics and SR_HLE_DIAGNOSTICS is present;
 * generic and public fixture profiles always return 0. */
int sr_title_config_diagnostics_enabled(void);

/* Configured fallback entry, or 0 when the build has none. 0 is not a usable guest
 * entry here, and the manifest validator rejects a configured zero, so callers can
 * treat 0 as "unconfigured" without a second predicate. */
uint32_t sr_title_config_fallback_entry(void);

/* Role predicates. Both answer 0 for every entry -- including entry 0 -- when the
 * corresponding binding is unconfigured, so an unconfigured build can never match a
 * role by accident. */
int sr_title_config_is_worker_entry(uint32_t entry);
int sr_title_config_is_launcher_entry(uint32_t entry);

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


/* How a guest PRX module declared by the title manifest is placed in guest memory. */
typedef enum SrGuestModulePlacement {
    SR_GUEST_MODULE_FIXED = 0,    /* translated and loaded at its manifest base */
    SR_GUEST_MODULE_RUNTIME = 1   /* no manifest base: translated position-independently and
                                     placed by the guest allocator when the game loads it */
} SrGuestModulePlacement;

/* Guest PRX module declared by the title manifest for this guest load path
 * (case-insensitive exact match, e.g. "disc0:/PSP_GAME/USRDIR/module/psmf.prx"):
 * its module file name, its placement, and -- for a fixed module -- the base both the
 * recompiler and runtime use (0 for a runtime-placed module). 0 = none. Every out
 * pointer may be NULL. */
int sr_title_config_guest_module(const char *guest_path, const char **name_out,
                                 uint32_t *base_out, int *required_out,
                                 SrGuestModulePlacement *placement_out);
unsigned sr_title_config_guest_module_count(void);
int sr_title_config_guest_module_at(unsigned index, const char **name_out, const char **guest_path_out,
                                    uint32_t *base_out, int *required_out,
                                    SrGuestModulePlacement *placement_out);

/* Expected extracted-data file-count census. Returns 0 when unconfigured
 * (generic build, or manifest without census), otherwise the exact count. */
uint32_t sr_title_config_expected_data_file_count(void);

/* Fired-patch report (#1274). Every site that applies a temporary compatibility patch
 * from `python tools/compat_overrides.py --debt-census` calls sr_compat_fired with the
 * three parts of that entry's census id: its manifest collection, its anchor address
 * and its name. The report prints them as "<collection>:0x%08x:<name>", which is the
 * census id itself, so tools/test_compat_manifest.py can hold the two lists equal.
 *
 * A site passes the anchor it already has -- a configured binding or, in generated
 * code, the patched address -- so this adds no title address to generic code. Only the
 * run log sees the report; nothing reaches the player's screen. Guest code runs on one
 * host thread, so the table is not locked. */
void sr_compat_fired(const char *collection, uint32_t anchor, const char *name);

/* Distinct entries that fired so far, in first-hit order. sr_compat_fired_at fills the
 * census id (truncated to id_size) and the hit count of the index-th one, returning 0
 * past the end. */
unsigned sr_compat_fired_count(void);
int sr_compat_fired_at(unsigned index, char *id, unsigned id_size, unsigned long long *hits);

/* End-of-run report to stderr: one COMPAT_PATCH_FIRED line per entry, then a
 * COMPAT_PATCH_REPORT summary that says "none" outright when nothing fired. Runs once
 * per process however many exit paths reach it. */
void sr_compat_report(void);

#endif /* SR_TITLE_CONFIG_H */
