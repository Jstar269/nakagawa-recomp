/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * The single translation unit that knows a title configuration exists.
 *
 * It binds the build-local artifact emitted by tools/title_runtime_config.py to the
 * generic SrTitleRuntimeConfig interface. Every other runtime source consumes the
 * accessors in title_config.h and never sees a title address literal, a generated
 * macro name, or a conditional compilation branch.
 */

#include "title_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build-local generated artifact (build/<game>/sr_title_config.h). The build always
 * generates it: with no title configuration it defines the generic all-disabled
 * configuration, so this include is unconditional and a missing artifact is a build
 * failure rather than a silent fallback to some other title's behavior. */
#include "sr_title_config.h"

#if SR_TITLE_CONFIG_SCHEMA_VERSION != 11
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

/* Guest PRX modules: the same manifest list the recompiler used for GAME_EXTRA_ELFS, so code
 * and data agree on one placement per module -- one address for a fixed module, the guest
 * allocator's choice for a runtime-placed one. Required modules fail closed when the staged
 * image is absent; optional modules may be supplied by host HLE. */
typedef struct {
    const char *name;
    const char *guest_path;
    uint32_t base;
    int required;
    SrGuestModulePlacement placement;
} SrTitleGuestModule;
#define SR_TITLE_CFG_GUEST_MODULE(n, p, b, r, pl) { (n), (p), (b), (r), (SrGuestModulePlacement)(pl) },
static const SrTitleGuestModule s_guest_modules[] = {
    SR_TITLE_CONFIG_GUEST_MODULE_LIST
    { "", "", 0u, 0, SR_GUEST_MODULE_FIXED }  /* placeholder: never read; the count is the authority */
};
#undef SR_TITLE_CFG_GUEST_MODULE
_Static_assert(sizeof s_guest_modules / sizeof s_guest_modules[0]
                   == SR_TITLE_CONFIG_GUEST_MODULE_COUNT + 1u,
               "generated guest-module list does not match its declared count");

static int ascii_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

int sr_title_config_guest_module(const char *guest_path, const char **name_out,
                                 uint32_t *base_out, int *required_out,
                                 SrGuestModulePlacement *placement_out) {
    if (!guest_path) return 0;
    /* The list ends at the placeholder (empty name); scanning to it rather than
     * comparing against the count keeps a zero-module build free of a constant
     * always-false loop condition (-Wtype-limits). */
    for (const SrTitleGuestModule *m = s_guest_modules; m->name[0]; m++) {
        if (m->guest_path[0] && ascii_ieq(m->guest_path, guest_path)) {
            if (name_out) *name_out = m->name;
            if (base_out) *base_out = m->base;
            if (required_out) *required_out = m->required;
            if (placement_out) *placement_out = m->placement;
            return 1;
        }
    }
    return 0;
}

unsigned sr_title_config_guest_module_count(void) {
    unsigned n = 0;
    while (s_guest_modules[n].name[0]) n++;
    return n;
}

int sr_title_config_guest_module_at(unsigned index, const char **name_out, const char **guest_path_out,
                                    uint32_t *base_out, int *required_out,
                                    SrGuestModulePlacement *placement_out) {
    if (index >= sr_title_config_guest_module_count()) return 0;
    if (name_out) *name_out = s_guest_modules[index].name;
    if (guest_path_out) *guest_path_out = s_guest_modules[index].guest_path;
    if (base_out) *base_out = s_guest_modules[index].base;
    if (required_out) *required_out = s_guest_modules[index].required;
    if (placement_out) *placement_out = s_guest_modules[index].placement;
    return 1;
}

static const SrTitleRuntimeConfig s_config = {
    SR_TITLE_CONFIG_VALID,
    SR_TITLE_CONFIG_FALLBACK_ENTRY,
    SR_TITLE_CONFIG_WORKER_THREAD_ENTRY,
    SR_TITLE_CONFIG_LAUNCHER_THREAD_ENTRY,
    SR_TITLE_CONFIG_EXPECTED_DATA_FILE_COUNT,
    SR_TITLE_CONFIG_SOURCE_ID,
    s_dispatch_aliases,
    (unsigned)SR_TITLE_CONFIG_DISPATCH_ALIAS_COUNT,
    s_callback_terminators,
    (unsigned)SR_TITLE_CONFIG_CALLBACK_TERMINATOR_COUNT,
};

const SrTitleRuntimeConfig *sr_title_config(void) { return &s_config; }

int sr_title_config_diagnostics_enabled(void) {
    /* The profile bit is emitted only from the validated manifest's bounded
     * codegen_profile field. The environment remains an explicit operator opt-in,
     * so a generic or public fixture build cannot activate the HST-only diagnostic
     * reads merely because a diagnostics environment leaked into its process. */
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (SR_TITLE_CONFIG_DIAGNOSTICS_PROFILE != 0) &&
                  (s_config.valid != 0u) &&
                  (getenv("SR_HLE_DIAGNOSTICS") != NULL);
    }
    return enabled;
}

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
        /* A match is the firing: dispatch reports completion on every 1 returned here.
         * The census anchors a terminator on its pc constraint, else its ra. */
        sr_compat_fired("TITLE_CONFIGURED_DISPATCH", t->has_pc ? t->pc : t->ra,
                        sentinel == 0u ? "callback-list walker null terminator"
                                       : "callback-list walker terminal miss");
        return 1;
    }
    return 0;
}

int sr_title_config_reent_bindings(SrTitleReentBindings *out) {
#if SR_TITLE_CONFIG_COMPAT_PROFILE
    /* #363 compatibility debt, HST only: live runtime behavior, not a diagnostic.
     * The validated manifest selects the flagship profile and its exact source id.
     * Keep this legacy guest-layout knowledge inside the title-config boundary. */
    if (s_config.valid != 0u && strcmp(s_config.source_id, "hst-ucus98701") == 0) {
        if (out) {
            out->master_reent_addr = 0x002cf338u;
            out->guest_thread_table_addr = 0x0030aa88u;
        }
        return 1;
    }
#else
    (void)out;
#endif
    return 0;
}

int sr_title_config_preserve_callee_saved_at_calls(void) {
#if SR_TITLE_CONFIG_COMPAT_PROFILE
    /* #363 compatibility debt, HST only: a returning dispatch CALL restores the
     * callee-saved $s0-$s7/$fp. Real hardware does not; a generic title sees every
     * register write its callee makes. Retire this once the flagship's clobber is
     * root-caused. */
    static int enabled = -1;
    if (enabled < 0)
        enabled = s_config.valid != 0u && strcmp(s_config.source_id, "hst-ucus98701") == 0;
    return enabled;
#else
    return 0;
#endif
}

uint32_t sr_title_config_expected_data_file_count(void) {
    if (!(s_config.valid & SR_TITLE_CFG_EXPECTED_DATA_FILE_COUNT)) return 0u;
    return s_config.expected_data_file_count;
}

/* Fired-patch report (#1274). 32 slots cover the frozen census (24 entries) with room;
 * hits on an entry past them still show in the summary's overflow count, never dropped
 * silently. */
#define SR_COMPAT_FIRED_MAX 32u
typedef struct {
    const char *collection;
    const char *name;
    uint32_t anchor;
    unsigned long long hits;
} SrCompatFired;
static SrCompatFired s_compat_fired[SR_COMPAT_FIRED_MAX];
static unsigned s_compat_fired_n;
static unsigned long long s_compat_fired_overflow;

void sr_compat_fired(const char *collection, uint32_t anchor, const char *name) {
    for (unsigned i = 0; i < s_compat_fired_n; i++) {
        SrCompatFired *e = &s_compat_fired[i];
        if (e->anchor != anchor) continue;
        if ((e->name == name || strcmp(e->name, name) == 0) &&
            (e->collection == collection || strcmp(e->collection, collection) == 0)) {
            e->hits++;
            return;
        }
    }
    if (s_compat_fired_n == SR_COMPAT_FIRED_MAX) {
        s_compat_fired_overflow++;
        return;
    }
    s_compat_fired[s_compat_fired_n++] = (SrCompatFired){ collection, name, anchor, 1u };
    /* The first hit goes to the log as it happens, so a run that dies before its
     * end-of-run report still shows which patches were active. */
    fprintf(stderr, "COMPAT_PATCH_FIRST_HIT: order=%u id=%s:0x%08x:%s\n",
            s_compat_fired_n, collection, (unsigned)anchor, name);
}

unsigned sr_compat_fired_count(void) { return s_compat_fired_n; }

int sr_compat_fired_at(unsigned index, char *id, unsigned id_size, unsigned long long *hits) {
    if (index >= s_compat_fired_n) return 0;
    const SrCompatFired *e = &s_compat_fired[index];
    if (id && id_size) snprintf(id, id_size, "%s:0x%08x:%s", e->collection, (unsigned)e->anchor, e->name);
    if (hits) *hits = e->hits;
    return 1;
}

void sr_compat_report(void) {
    static int reported;
    if (reported) return;
    reported = 1;
    char id[256];
    unsigned long long hits;
    for (unsigned i = 0; sr_compat_fired_at(i, id, sizeof id, &hits); i++)
        fprintf(stderr, "COMPAT_PATCH_FIRED: order=%u hits=%llu id=%s\n", i + 1u, hits, id);
    if (s_compat_fired_n == 0u)
        fprintf(stderr, "COMPAT_PATCH_REPORT: fired=0 none (title %s)\n", s_config.source_id);
    else
        fprintf(stderr, "COMPAT_PATCH_REPORT: fired=%u overflow_hits=%llu (title %s)\n",
                s_compat_fired_n, s_compat_fired_overflow, s_config.source_id);
    fflush(stderr);
}
