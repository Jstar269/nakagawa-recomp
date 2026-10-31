// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Utility dialogs and modules: sceUtility and sceImpose. Extracted from hle.c on 2026-10-31
 * (#307). */

#include "hle_internal.h"

static uint32_t h_GetSystemParamInt(CpuState *s) {
    uint32_t id = A0, out = A1;
    /* Measured on the PSP-3000 oracle (2026-10-10, hle-sysparam): ids 2 to 10 answer; any other id
     * (0 and 64 were tried) returns PSP_SYSTEMPARAM_RETVAL_FAIL and leaves the output word untouched. */
    if (id < 2u || id > 10u) return 0x80110103u;
    /* Read through the virtual system registry, so a setting a game wrote there is what this
     * getter reports: the two surfaces share one value per modeled key. */
    uint32_t v = sreg_systemparam_int(id);
    if (out) MEM_W32(out, v);
    if (hle_log_on()) fprintf(stderr, "sceUtilityGetSystemParamInt: id=%u -> %u\n", id, v);
    return 0;
}

/* sceUtilityGetSystemParamString(int id, char *str, int len) (PSPSDK psputility_sysparam.h): 0 on
 * success, PSP_SYSTEMPARAM_RETVAL_FAIL (0x80110103) on failure. The nickname is id 1 and reads the
 * registry's /CONFIG/SYSTEM/owner_name value, so it is the neutral empty string unless a game
 * wrote one; no user name is modelled or invented. len is the buffer length. Measured on the
 * PSP-3000 oracle (2026-10-10, hle-sysparam): a buffer that cannot hold the string and its NUL
 * fails with 0x80110102 and writes nothing; a large enough buffer receives the string and its NUL
 * and the call returns 0. Other string ids, a null buffer and len <= 0 fail with 0x80110103. */
#define SYSPARAM_RETVAL_FAIL 0x80110103u
#define SYSPARAM_RETVAL_BUFSIZE 0x80110102u
#define SYSPARAM_STRING_NICKNAME 1u
static uint32_t h_GetSystemParamString(CpuState *s) {
    uint32_t id = A0, out = A1;
    int32_t len = (int32_t)A2;
    uint32_t count = 0u;
    int node;
    SregNode *n;
    if (id != SYSPARAM_STRING_NICKNAME || !out || len <= 0) return SYSPARAM_RETVAL_FAIL;
    sreg_ensure_loaded();
    node = sreg_find_path("/CONFIG/SYSTEM/owner_name");
    if (node < 0) return SYSPARAM_RETVAL_FAIL;
    n = &s_sreg_nodes[node];
    if (n->type != SREG_TYPE_STR) return SYSPARAM_RETVAL_FAIL;
    while (count < n->size && n->value[count]) count++;
    if (count + 1u > (uint32_t)len) return SYSPARAM_RETVAL_BUFSIZE;
    if (!sr_guest_span_writable(out, count + 1u)) return SYSPARAM_RETVAL_FAIL;
    sreg_note_placeholder_read(node, "sceUtilityGetSystemParamString");
    for (uint32_t i = 0; i < count; i++) MEM_W8(out + i, n->value[i]);
    MEM_W8(out + count, 0u);
    return 0u;
}

/* sceImpose: the impose language and confirm-button mode. The game sets this once during boot and
 * the impose surface (plus any dialog that follows the system convention) must agree with it, so
 * this is retained state rather than the h_ok fake success it used to be.
 *
 * Contract (public reference model: PPSSPP Core/HLE/sceImpose.cpp; the NIDs below are the same
 * ones already carried by src/rt/nid_names.h):
 *   sceImposeSetLanguageMode(language, buttonConfirm) stores BOTH values and returns 0. The
 *     reference also stores a language that differs from the system language -- it logs a warning
 *     and still returns 0 -- so a mismatch must not become an error here.
 *   sceImposeGetLanguageMode(int *language, int *buttonConfirm) writes both back and returns 0.
 *     Either out-pointer is optional in the wrapper ('xx'), so each is written only when the guest
 *     supplied a writable 4-byte span.
 * Defaults come from the same source as sceUtilityGetSystemParamInt (the system registry over the
 * system-param table), so the two surfaces cannot disagree before the game's first set. The
 * numeric convention for the confirm button is whatever that source reports for id 9; it is
 * deliberately not reinterpreted here. */
static uint32_t s_impose_language;
static uint32_t s_impose_button;
static int s_impose_inited;

static void impose_init_once(void) {
    if (s_impose_inited) return;
    s_impose_inited = 1;
    s_impose_language = sreg_systemparam_int(8);  /* SCE_SYSTEMPARAM_ID_INT_LANGUAGE */
    s_impose_button = sreg_systemparam_int(9);    /* SCE_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE */
}

static uint32_t h_ImposeSetLanguageMode(CpuState *s) {
    (void)s;
    impose_init_once();
    s_impose_language = A0;
    s_impose_button = A1;
    if (hle_log_on())
        fprintf(stderr, "sceImposeSetLanguageMode: language=%u buttonConfirm=%u\n", A0, A1);
    return 0;
}

static uint32_t h_ImposeGetLanguageMode(CpuState *s) {
    (void)s;
    impose_init_once();
    if (A0 && sr_guest_span_writable(A0, 4u)) MEM_W32(A0, s_impose_language);
    if (A1 && sr_guest_span_writable(A1, 4u)) MEM_W32(A1, s_impose_button);
    if (hle_log_on())
        fprintf(stderr, "sceImposeGetLanguageMode: -> language=%u buttonConfirm=%u\n",
                s_impose_language, s_impose_button);
    return 0;
}

/* sceUtility */
/* AV module IDs are firmware utility IDs, not aliases for the title's private
 * libfont/PSMF PRXs.  Keep this state separate from the explicit PRX loader
 * (h_LoadModule/h_LoadModuleByID), which is the only path that knows a PRX's
 * concrete export table and load address. */
enum {
    PSP_AV_MODULE_AVCODEC = 0x300,
    PSP_AV_MODULE_SASCORE = 0x301,
    PSP_AV_MODULE_ATRAC3PLUS = 0x302,
    PSP_AV_MODULE_MPEGBASE = 0x303,
    PSP_AV_MODULE_MP3 = 0x304,
    PSP_AV_MODULE_VAUDIO = 0x305,
    PSP_AV_MODULE_AAC = 0x306,
    PSP_AV_MODULE_G729 = 0x307,
    PSP_AV_MODULE_MP4 = 0x308,
};
#define SCE_ERROR_MODULE_NOT_LOADED         0x80111103u
#define SCE_ERROR_AV_MODULE_BAD_ID         0x80110f01u
#define SCE_ERROR_AV_MODULE_ALREADY_LOADED 0x80110f02u
#define SCE_ERROR_AV_MODULE_NOT_LOADED     0x80110f03u
#define SCE_ERROR_AV_LIBRARY_NOT_FOUND      0x8002013cu
static unsigned s_utility_usb_loaded;

/* The firmware utility modules sceUtilityLoadModule knows, with the modules each one needs
 * loaded first.  IDs, names and dependencies follow PPSSPP Core/HLE/sceUtility.cpp moduleLoadInfo
 * (3f40ddc8); an ID outside the table is BAD_ID, and a dependency that is not loaded yet is
 * LIBRARY_NOT_FOUND (Kamen Rider Climax Heroes OOO relies on that error, per the same source).
 * Loading records module state only: the host HLE serves every library these modules provide,
 * and loading one never touches a network (the net libraries themselves stay fail-closed).  The
 * user-partition memory the real modules occupy is not reserved (unmeasured per module). */
typedef struct {
    uint16_t id;
    const char *name;
    uint16_t deps[5];
} UtilityModuleInfo;
static const UtilityModuleInfo s_utility_modules[] = {
    {0x100, "net_common", {0}},
    {0x101, "net_adhoc", {0}},
    {0x102, "net_inet", {0}},
    {0x103, "net_parse_uri", {0}},
    {0x104, "net_parse_http", {0}},
    {0x105, "net_http", {0x102, 0x103, 0x104, 0}},
    {0x106, "net_ssl", {0x102, 0}},
    {0x107, "unk_0x107", {0}},
    {0x108, "usb_pspcm", {0x100, 0x102, 0x103, 0x104, 0x105}},
    {0x200, "usb_mic", {0}},
    {0x201, "usb_cam", {0}},
    {0x202, "usb_gps", {0}},
    {0x203, "usb_unk_0x203", {0}},
    {0x2ff, "unk_0x2ff", {0}},
    {PSP_AV_MODULE_AVCODEC, "av_avcodec", {0}},
    {PSP_AV_MODULE_SASCORE, "av_sascore", {0}},
    {PSP_AV_MODULE_ATRAC3PLUS, "av_atrac3plus", {PSP_AV_MODULE_AVCODEC, 0}},
    {PSP_AV_MODULE_MPEGBASE, "av_mpegbase", {PSP_AV_MODULE_AVCODEC, 0}},
    {PSP_AV_MODULE_MP3, "av_mp3", {0}},
    {PSP_AV_MODULE_VAUDIO, "av_vaudio", {0}},
    {PSP_AV_MODULE_AAC, "av_aac", {0}},
    {PSP_AV_MODULE_G729, "av_g729", {0}},
    {PSP_AV_MODULE_MP4, "av_mp4", {PSP_AV_MODULE_AVCODEC, 0}},
    {0x3fe, "me_stuff", {0}},
    {0x3ff, "me_core", {0}},
    {0x400, "np_common", {0}},
    {0x401, "np_service", {0}},
    {0x402, "np_matching2", {0}},
    {0x403, "np_unk_0x403", {0}},
    {0x500, "np_drm", {0}},
    {0x600, "irda", {0}},
    {0x601, "unk_0x601", {0}},
};
#define UTILITY_MODULE_COUNT (sizeof(s_utility_modules) / sizeof(s_utility_modules[0]))
static uint8_t s_utility_module_loaded[UTILITY_MODULE_COUNT];

static int utility_module_index(uint32_t module) {
    for (size_t i = 0; i < UTILITY_MODULE_COUNT; i++)
        if (s_utility_modules[i].id == module) return (int)i;
    return -1;
}

static uint32_t utility_module_load(const char *api, uint32_t module, int av_api) {
    int index = utility_module_index(module);
    if (index < 0)
        return av_api ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID;
    if (s_utility_module_loaded[index])
        return av_api ? SCE_ERROR_AV_MODULE_ALREADY_LOADED : SCE_ERROR_MODULE_ALREADY_LOADED;
    const UtilityModuleInfo *info = &s_utility_modules[index];
    for (size_t d = 0; d < sizeof(info->deps) / sizeof(info->deps[0]) && info->deps[d]; d++) {
        int dep = utility_module_index(info->deps[d]);
        if (dep < 0 || !s_utility_module_loaded[dep]) return SCE_ERROR_AV_LIBRARY_NOT_FOUND;
    }
    s_utility_module_loaded[index] = 1u;
    fprintf(stderr, "%s: recognized %s (utility module state loaded)\n", api, info->name);
    fflush(stderr);
    return 0;
}

static uint32_t utility_module_unload(const char *api, uint32_t module, int av_api) {
    int index = utility_module_index(module);
    if (index < 0)
        return av_api ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID;
    if (!s_utility_module_loaded[index])
        return av_api ? SCE_ERROR_AV_MODULE_NOT_LOADED : SCE_ERROR_MODULE_NOT_LOADED;
    s_utility_module_loaded[index] = 0u;
    fprintf(stderr, "%s: recognized %s (utility module state unloaded)\n", api,
            s_utility_modules[index].name);
    fflush(stderr);
    return 0;
}

static uint32_t h_UtilityLoadModule(CpuState *s) {
    uint32_t module = s->r[4];
    fprintf(stderr, "sceUtilityLoadModule: module=0x%08x\n", module);
    fflush(stderr);
    /* Utility AV loads are host-side module-state notifications.  Do not run
     * the title's private PRX module_start here: those entry points assume a
     * PSP kernel and are intentionally skipped.  The private export tables
     * are registered only by the concrete kernel PRX loader below. */
    /* A bare low byte is not a valid reason to publish one of the private
     * title PRX tables. Those exports are registered by the concrete kernel
     * module-load path instead. */
    return utility_module_load("sceUtilityLoadModule", module, 0);
}
static uint32_t h_UtilityUnloadModule(CpuState *s) {
    return utility_module_unload("sceUtilityUnloadModule", s->r[4], 0);
}
static uint32_t h_UtilityLoadAvModule(CpuState *s) {
    uint32_t module = s->r[4];
    if (module > 7u) return SCE_ERROR_AV_MODULE_BAD_ID;
    return utility_module_load("sceUtilityLoadAvModule", PSP_AV_MODULE_AVCODEC + module, 1);
}
static uint32_t h_UtilityUnloadAvModule(CpuState *s) {
    uint32_t module = s->r[4];
    if (module > 7u) return SCE_ERROR_AV_MODULE_BAD_ID;
    return utility_module_unload("sceUtilityUnloadAvModule", PSP_AV_MODULE_AVCODEC + module, 1);
}
/* sceUtilityLoadNetModule(n) is the older form of loading a net module: PSPSDK's
 * psputility_netmodules.h numbers them 1 (COMMON) to 7 (SSL) in the order of the 0x100..0x106
 * utility IDs, and the state is shared with sceUtilityLoadModule.  The status codes for a bad
 * number or a repeated call are the generic module ones here; unmeasured on a PSP. */
static uint32_t h_UtilityLoadNetModule(CpuState *s) {
    uint32_t module = s->r[4];
    if (module < 1u || module > 7u) return SCE_ERROR_MODULE_BAD_ID;
    return utility_module_load("sceUtilityLoadNetModule", 0x100u + module - 1u, 0);
}
static uint32_t h_UtilityUnloadNetModule(CpuState *s) {
    uint32_t module = s->r[4];
    if (module < 1u || module > 7u) return SCE_ERROR_MODULE_BAD_ID;
    return utility_module_unload("sceUtilityUnloadNetModule", 0x100u + module - 1u, 0);
}

/* sceUtilityLoadUsbModule(n) numbers its modules 1 to 5 (PSPSDK psputility_usbmodules.h:
 * PSPCM, ACC, MIC, CAM, GPS), an order that does not follow the 0x108 and 0x2xx IDs of the
 * utility-module table above, and which table ID each loads is unmeasured, so their state is
 * kept here rather than guessed into the table. The USB functions themselves are not provided.
 * The error mapping is inferred from the generic utility-module API, not measured. */
static uint32_t utility_usb_module(uint32_t id, int load) {
    if (id < 1u || id > 5u) return SCE_ERROR_MODULE_BAD_ID;
    unsigned bit = 1u << (id - 1u);
    if (load) {
        if (s_utility_usb_loaded & bit) return SCE_ERROR_MODULE_ALREADY_LOADED;
        s_utility_usb_loaded |= bit;
    } else {
        if (!(s_utility_usb_loaded & bit)) return SCE_ERROR_MODULE_NOT_LOADED;
        s_utility_usb_loaded &= ~bit;
    }
    return 0;
}
static uint32_t h_UtilityLoadUsbModule(CpuState *s) { return utility_usb_module(A0, 1); }
static uint32_t h_UtilityUnloadUsbModule(CpuState *s) { return utility_usb_module(A0, 0); }

/* sceUtilitySavedata: real persistence on a virtual memory stick (src/rt/savedata.c). */
uint32_t sr_savedata_execute_sized(uint32_t param, uint32_t size);
uint32_t sr_savedata_prepare_utility(unsigned kind);

enum { SR_UTILITY_STORAGE_GAMEDATA = 1, SR_UTILITY_STORAGE_GAMESHARING = 2 };

/* One utility dialog slot (#1386). Savedata, message, netconf, gamedata-install, game-sharing
 * and the OSK share it: only one utility dialog runs at a time. InitStart takes the slot only
 * while the dialog holding it is back at NONE, and the type that took it keeps it after it
 * winds down, so that type's own GetStatus then reports NONE (0) rather than WRONG_TYPE: a
 * game spinning "while (sceUtilityOskGetStatus() != 0)" after name entry would otherwise wait
 * forever. A call for any other type returns WRONG_TYPE (0x80110005), the OSK's PSP-measured
 * result with no keyboard (#363), which the #1386 contract applies to every dialog. An
 * InitStart while the holder is still active returns INVALID_STATUS (0x80110001) and changes
 * nothing: the code the #1386 contract names, and the OSK's measured code for a call in the
 * wrong state. What a PSP returns for a second InitStart is unmeasured, so the slot fails
 * closed rather than let a new dialog replace one that is still running. */
#define SCE_ERROR_UTILITY_INVALID_STATUS 0x80110001u
#define SCE_ERROR_UTILITY_WRONG_TYPE     0x80110005u
static int utility_slot_active_locked(void);    /* fwd: asks the holder's own state machine */
/* Take the slot for `type`, or refuse while its holder is active. Caller holds dialog_lock. */
static uint32_t utility_slot_claim_locked(int type) {
    if (utility_slot_active_locked()) {
        if (getenv("SR_DLGLOG"))
            fprintf(stderr, "utility InitStart (type %d) refused: type %d holds the active slot\n",
                    type, s_utility_slot);
        return SCE_ERROR_UTILITY_INVALID_STATUS;
    }
    s_utility_slot = type;
    return 0;
}
/* A busy slot is reported before the parameter block is looked at: on a PSP a game-sharing
 * InitStart with a bad size returns INVALID_STATUS while a savedata dialog is starting, and
 * INVALID_PARAM_SIZE once nothing is active (PSPAutotests tests/utility/dialog/status.expected,
 * hrydgard/pspautotests master). A refused block never takes the slot: every type still reports
 * WRONG_TYPE after it. Measured for game sharing; the other dialogs share the one slot and are
 * checked in the same order. */
static int utility_slot_busy(void) {
    dialog_lock();
    const int busy = utility_slot_active_locked();
    dialog_unlock();
    return busy;
}
/* The block's first word is its size, one of `count` versioned sizes; the whole block must be
 * readable and writable (PSPAutotests tests/utility/dialog/sizes.expected). */
static int utility_param_size_ok(uint32_t param, const uint32_t *sizes, size_t count) {
    if (!param || !sr_guest_span_readable(param, 4u)) return 0;
    const uint32_t size = MEM_R32(param);
    for (size_t i = 0; i < count; i++)
        if (size == sizes[i])
            return sr_guest_span_readable(param, size) && sr_guest_span_writable(param, size);
    return 0;
}
uint32_t s_dlg_param = 0, s_dlg_param_size = 0, s_dlg_result = 0, s_dlg_generation = 0;
unsigned s_dlg_work_started, s_dlg_work_done;
static uint32_t h_SavedataInitStart(CpuState *s) {
    /* The block's first word is its size: the 0x5c8, 0x5dc and 0x600-byte versions are
     * accepted and any other size is refused (pspautotests e2d098fc), after the busy check. */
    static const uint32_t sizes[] = { 0x5c8u, 0x5dcu, 0x600u };
    if (utility_slot_busy()) return SCE_ERROR_UTILITY_INVALID_STATUS;
    if (!utility_param_size_ok(A0, sizes, sizeof(sizes) / sizeof(sizes[0]))) return 0x80110004u;
    const uint32_t size = MEM_R32(A0);
    dialog_lock();
    const uint32_t claim = utility_slot_claim_locked(UTILITY_SLOT_SAVEDATA);
    if (claim) {
        dialog_unlock();
        return claim;
    }
    s_dlg_param = A0; s_dlg_param_size = size; s_dlg_status = 1; s_dlg_tick = 0;
    s_dlg_work_started = s_dlg_work_done = 0;
    s_dlg_generation++;
    s_dlg_result = 0;
    MEM_W32(A0 + 0x1c, 0);
    dialog_unlock();
    if (getenv("SR_DLGLOG")) { static int n=0; fprintf(stderr, "** SavedataInitStart #%d **\n", ++n);
        uint32_t p = A0;
        char gn[16], sn[24];
        if (!guest_cstr(p+0x3c, gn, sizeof(gn))) gn[0] = '\0';
        if (!guest_cstr(p+0x4c, sn, sizeof(sn))) sn[0] = '\0';
        fprintf(stderr, "SavedataInitStart param=0x%08x size=%u mode=%d gameName='%s' saveName='%s' result=0x%08x\n",
            p, MEM_R32(p+0), (int)MEM_R32(p+0x30), gn, sn, s_dlg_result);
    }
    return 0;
}
static uint32_t h_DlgGetStatus(CpuState *s) {
    (void)s;
    dialog_lock();
    if (s_utility_slot != UTILITY_SLOT_SAVEDATA) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    int ret = s_dlg_status;
    if (s_dlg_status == 1) {              /* INIT reported: start-up completes, RUNNING */
        s_dlg_status = 2; s_dlg_tick = 0;
    } else if (s_dlg_status == 4) {       /* SHUTDOWN reported: the shutdown completes, NONE */
        s_dlg_status = 0;
    }
    if (getenv("SR_DLGLOG")) {                            /* log transitions only (unbounded) */
        static int last = -1;
        if (s_dlg_status != last) {
            fprintf(stderr, "DlgGetStatus: ret=%d -> status=%d (result=0x%08x)\n", ret, s_dlg_status, s_dlg_result);
            last = s_dlg_status;
        }
    }
    dialog_unlock();
    return (uint32_t)ret;
}
static uint32_t h_SavedataUpdate(CpuState *s) {
    (void)s;
    uint32_t param = 0, size = 0, generation = 0;
    dialog_lock();
    if (s_utility_slot != UTILITY_SLOT_SAVEDATA) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    if (s_dlg_status == 0 || s_dlg_status == 1) { /* NONE and INIT reject Update (#1386) */
        dialog_unlock();
        return SCE_ERROR_UTILITY_INVALID_STATUS;
    }
    if (s_dlg_status == 2 && !s_dlg_work_started) {
        s_dlg_work_started = 1;
        param = s_dlg_param;
        size = s_dlg_param_size;
        generation = s_dlg_generation;
    } else if (s_dlg_status == 2 && s_dlg_work_done && ++s_dlg_tick >= 2) {
        s_dlg_status = 3;
        if (s_dlg_param) MEM_W32(s_dlg_param + 0x1c, s_dlg_result);
        if (getenv("SR_DLGLOG")) fprintf(stderr, "SavedataUpdate: FINISHED result=0x%08x written to 0x%08x\n",
                                         s_dlg_result, s_dlg_param + 0x1c);
    }
    dialog_unlock();
    if (param) {
        uint32_t result = sr_savedata_execute_sized(param, size);
        dialog_lock();
        if (generation == s_dlg_generation) {
            s_dlg_result = result;
            s_dlg_work_done = 1;
        }
        dialog_unlock();
    }
    return 0;
}
/* A savedata shutdown ends on a priority-0x20 thread, whatever the dialog's own thread
 * priorities (PSPAutotests tests/utility/savedata/shutdownstatus.expected, SIZES on a save
 * that exists and on one that does not): a caller at 0x20 keeps the CPU, so its next GetStatus
 * reports SHUTDOWN and a later one NONE, while a caller at 0x21-0x24 or 0x6F is preempted until
 * the shutdown is over and sees only NONE. Such a caller can therefore start the next dialog
 * straight after ShutdownStart. A caller better than 0x20 is unmeasured and sees SHUTDOWN, as
 * one at 0x20 does. Only a shutdown from FINISHED is measured; ShutdownStart during INIT or
 * RUNNING keeps the reported SHUTDOWN. */
#define UTILITY_SAVEDATA_SHUTDOWN_PRIORITY 0x20
static uint32_t h_DlgShutdown(CpuState *s) {
    (void)s;
    const int caller_priority = sched_current_priority();
    dialog_lock();
    if (s_utility_slot != UTILITY_SLOT_SAVEDATA) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    /* PSPAutotests tests/utility/dialog/status.expected: repeated shutdown and
     * shutdown after NONE return INVALID_STATUS. */
    if (s_dlg_status == 0 || s_dlg_status == 4) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_INVALID_STATUS;
    }
    if (s_dlg_status == 3 && caller_priority > UTILITY_SAVEDATA_SHUTDOWN_PRIORITY)
        s_dlg_status = 0;                /* the shutdown ran before the caller got the CPU back */
    else
        s_dlg_status = 4;
    dialog_unlock();
    if (getenv("SR_DLGLOG")) fprintf(stderr, "DlgShutdownStart\n");
    return 0;
}
/* The common-block result an aborted dialog finishes with (see utility_dialog_abort). */
#define UTILITY_DIALOG_RESULT_ABORT 2u
enum {  /* a dialog's kind is the slot type it claims */
    UTILITY_MSG = UTILITY_SLOT_MSG, UTILITY_NETCONF = UTILITY_SLOT_NETCONF,
    UTILITY_GAMEDATA = UTILITY_SLOT_GAMEDATA, UTILITY_GAMESHARING = UTILITY_SLOT_GAMESHARING
};
UtilityDialog s_msg_dialog = {.kind = UTILITY_MSG};
UtilityDialog s_net_dialog = {.kind = UTILITY_NETCONF};
UtilityDialog s_gamedata_dialog = {.kind = UTILITY_GAMEDATA};
UtilityDialog s_sharing_dialog = {.kind = UTILITY_GAMESHARING};

/* `param` has passed its type's size check, which follows the busy check (utility_slot_busy). */
static uint32_t utility_dialog_init(UtilityDialog *d, uint32_t param) {
    if (!param) return 0x80110004u; /* SCE_ERROR_UTILITY_INVALID_PARAM */
    dialog_lock();
    const uint32_t claim = utility_slot_claim_locked((int)d->kind);
    if (claim) {
        dialog_unlock();
        return claim;
    }
    d->param = param;
    d->result = 0;
    d->generation++;
    d->status = 1;
    d->ticks = 0;
    d->work_started = d->work_done = d->aborted = 0;
    MEM_W32(param + 0x1c, 0);
    dialog_unlock();
    return 0;
}
/* The answer a message dialog confirmed at once gives: its default button. There is no message
 * dialog UI, so the runtime presses the button the cursor starts on, which is what a person
 * pressing X straight away gets. PSPSDK psputility_msgdialog.h: options bit 0x10 asks for
 * YES/NO buttons and bit 0x100 puts the cursor on NO; buttonPressed reads 1 for YES (and OK) and
 * 2 for NO. A "delete this save? (default No)" prompt is therefore answered No, never Yes. */
#define MSG_DIALOG_OPTION_YESNO      0x10u
#define MSG_DIALOG_OPTION_DEFAULT_NO 0x100u
static uint32_t msg_dialog_default_answer(uint32_t param) {
    const uint32_t options = MEM_R32(param + 0x23c);
    const int no = (options & MSG_DIALOG_OPTION_YESNO) && (options & MSG_DIALOG_OPTION_DEFAULT_NO);
    fprintf(stderr, "sceUtilityMsgDialog: no dialog UI; answered %s, the default button "
            "(options 0x%08" PRIx32 ")\n", no ? "NO" : "YES/OK", options);
    return no ? 2u : 1u;
}
static uint32_t utility_dialog_update(UtilityDialog *d) {
    uint32_t param = 0, generation = 0, result = 0;
    unsigned kind = 0;
    dialog_lock();
    if (s_utility_slot != (int)d->kind) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    /* Still starting up: Update does not move INIT (#1386). A message dialog also refuses
     * Update once it is back at NONE ("msg Update after done: INVALID_STATUS", PSPAutotests
     * tests/utility/dialog/status.expected); netconf, gamedata and sharing are unmeasured there
     * and keep accepting it. */
    if (d->status == 1 || (d->status == 0 && d->kind == UTILITY_MSG)) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_INVALID_STATUS;
    }
    if (d->status == 2 && d->aborted) {
        /* An aborted dialog finishes on the next Update (sceUtilityMsgDialogAbort). */
        d->status = 3;
        if (d->param) MEM_W32(d->param + 0x1c, UTILITY_DIALOG_RESULT_ABORT);
    } else if (d->status == 2 && !d->work_started) {
        d->work_started = 1;
        param = d->param;
        generation = d->generation;
        kind = d->kind;
    } else if (d->status == 2 && d->work_done && ++d->ticks >= 2) {
        d->status = 3;
        if (d->param) MEM_W32(d->param + 0x1c, d->result);
    }
    dialog_unlock();

    if (param) {
        if (kind == UTILITY_GAMEDATA)
            result = sr_savedata_prepare_utility(SR_UTILITY_STORAGE_GAMEDATA);
        else if (kind == UTILITY_GAMESHARING)
            result = sr_savedata_prepare_utility(SR_UTILITY_STORAGE_GAMESHARING);
        /* The message dialog answers with its default button (msg_dialog_default_answer). The
         * answer goes to buttonPressed at +0x240, which exists from the 580-byte (V2) parameter
         * block on; +0x23c is the caller's options word (YES/NO, default NO, ...) and is
         * input, never written (PPSSPP Core/Dialog/PSPMsgDialog.h pspMessageDialog: V1 ends
         * at 572 bytes, V2 adds options and buttonPressed, V3 is 708 bytes). Writing the
         * answer into options left buttonPressed 0, which a yes/no caller reads as "no". */
        else if (kind == UTILITY_MSG && MEM_R32(param) >= 0x244u)
            MEM_W32(param + 0x240, msg_dialog_default_answer(param));
        dialog_lock();
        if (generation == d->generation) {
            d->result = result;
            d->work_done = 1;
        }
        dialog_unlock();
    }
    return 0;
}
static uint32_t utility_dialog_status(UtilityDialog *d) {
    dialog_lock();
    if (s_utility_slot != (int)d->kind) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    uint32_t ret = (uint32_t)d->status;
    if (d->status == 1) {
        d->status = 2;
        d->ticks = 0;
    } else if (d->status == 4) {
        d->status = 0;
        d->param = 0;
        d->ticks = 0;
        d->work_started = d->work_done = 0;
    }
    dialog_unlock();
    return ret;
}
static uint32_t utility_dialog_shutdown(UtilityDialog *d) {
    dialog_lock();
    if (s_utility_slot != (int)d->kind) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    /* A message dialog shuts down only from FINISHED: ShutdownStart during INIT, while RUNNING and
     * again after it are INVALID_STATUS (PSPAutotests tests/utility/dialog/status.expected). The
     * other types are unmeasured and keep the old rule. */
    if (d->kind == UTILITY_MSG ? d->status != 3 : !d->status) {
        dialog_unlock();
        return d->kind == UTILITY_MSG ? SCE_ERROR_UTILITY_INVALID_STATUS : 0u;
    }
    d->status = 4;
    dialog_unlock();
    return 0;
}
/* sceUtilityMsgDialogAbort(void). Measured on a PSP by pspautotests
 * tests/utility/msgdialog/abort.expected (rev 2ed7c9c5): before any InitStart it returns
 * 0x80110005; during INIT 0x80110001; while the dialog is visible 0, and again 0 a second time,
 * with GetStatus still 2; the next Update returns 0 and GetStatus then reads 3 (FINISHED); a third
 * Abort, one during or after ShutdownStart, and one after the dialog is back to NONE all return
 * 0x80110001. Here the slot keeps its last type after a shutdown, so "never started" is the slot
 * held by no message dialog (WRONG_TYPE) and every other state but RUNNING is INVALID_STATUS.
 * The result an aborted dialog writes to the common block is not in the capture: it is
 * SCE_UTILITY_DIALOG_RESULT_ABORT (2) as PPSSPP Core/Dialog writes it (a behaviour reference, not
 * a PSP measurement). An answer this model already wrote at the dialog's first Update (the default
 * button, utility_dialog_update) stays; what buttonPressed holds after an abort is not measured. */
static uint32_t utility_dialog_abort(UtilityDialog *d) {
    dialog_lock();
    if (s_utility_slot != (int)d->kind) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_WRONG_TYPE;
    }
    if (d->status != 2) {
        dialog_unlock();
        return SCE_ERROR_UTILITY_INVALID_STATUS;
    }
    d->aborted = 1;
    dialog_unlock();
    return 0;
}
static uint32_t h_MsgDialogAbort(CpuState *s);
/* PSPAutotests tests/utility/dialog/sizes.expected: the message dialog accepts the 0x23c (V1),
 * 0x244 (V2) and 0x2c4 (V3) blocks and every other size from 0 to 0x800 is INVALID_PARAM_SIZE;
 * larger sizes are unmeasured and refused the same way. */
static uint32_t h_MsgDialogInit(CpuState *s) {
    static const uint32_t sizes[] = { 0x23cu, 0x244u, 0x2c4u };
    if (utility_slot_busy()) return SCE_ERROR_UTILITY_INVALID_STATUS;
    if (!utility_param_size_ok(A0, sizes, sizeof(sizes) / sizeof(sizes[0]))) return 0x80110004u;
    return utility_dialog_init(&s_msg_dialog, A0);
}
#define UTILITY_DIALOG_HANDLERS(prefix, slot) \
    static uint32_t prefix##Update(CpuState *s) { (void)s; return utility_dialog_update(&(slot)); } \
    static uint32_t prefix##Status(CpuState *s) { (void)s; return utility_dialog_status(&(slot)); } \
    static uint32_t prefix##Shutdown(CpuState *s) { (void)s; return utility_dialog_shutdown(&(slot)); }
UTILITY_DIALOG_HANDLERS(h_MsgDialog, s_msg_dialog)
static uint32_t h_MsgDialogAbort(CpuState *s) { (void)s; return utility_dialog_abort(&s_msg_dialog); }
/* PSPAutotests utility/dialog/sizes lists these netconf parameter versions. */
static uint32_t h_NetDialogInit(CpuState *s) {
    static const uint32_t sizes[] = { 0x38u, 0x40u, 0x44u };
    if (utility_slot_busy()) return SCE_ERROR_UTILITY_INVALID_STATUS;
    if (!utility_param_size_ok(A0, sizes, sizeof(sizes) / sizeof(sizes[0]))) return 0x80110004u;
    return utility_dialog_init(&s_net_dialog, A0);
}
static uint32_t h_NetDialogUpdate(CpuState *s) { (void)s; return utility_dialog_update(&s_net_dialog); }
static uint32_t h_NetDialogStatus(CpuState *s) { (void)s; return utility_dialog_status(&s_net_dialog); }
static uint32_t h_NetDialogShutdown(CpuState *s) { (void)s; return utility_dialog_shutdown(&s_net_dialog); }
/* PSPAutotests utility/dialog/sizes lists the storage dialog versions. */
static uint32_t utility_storage_init(UtilityDialog *d, uint32_t param) {
    static const uint32_t gamedata[] = { 0x590u, 0x598u }, sharing[] = { 0x50u, 0x54u, 0x64u };
    const int install = d->kind == UTILITY_GAMEDATA;
    if (utility_slot_busy()) return SCE_ERROR_UTILITY_INVALID_STATUS;
    if (!utility_param_size_ok(param, install ? gamedata : sharing, install ? 2u : 3u))
        return 0x80110004u;
    return utility_dialog_init(d, param);
}
static uint32_t h_GamedataDialogInit(CpuState *s) { return utility_storage_init(&s_gamedata_dialog, A0); }
static uint32_t h_GamedataDialogUpdate(CpuState *s) { (void)s; return utility_dialog_update(&s_gamedata_dialog); }
static uint32_t h_GamedataDialogStatus(CpuState *s) { (void)s; return utility_dialog_status(&s_gamedata_dialog); }
static uint32_t h_GamedataDialogShutdown(CpuState *s) { (void)s; return utility_dialog_shutdown(&s_gamedata_dialog); }
static uint32_t h_SharingDialogInit(CpuState *s) { return utility_storage_init(&s_sharing_dialog, A0); }
static uint32_t h_SharingDialogUpdate(CpuState *s) { (void)s; return utility_dialog_update(&s_sharing_dialog); }
static uint32_t h_SharingDialogStatus(CpuState *s) { (void)s; return utility_dialog_status(&s_sharing_dialog); }
static uint32_t h_SharingDialogShutdown(CpuState *s) { (void)s; return utility_dialog_shutdown(&s_sharing_dialog); }
uint32_t s_osk_param = 0;
int s_osk_field = 0;           /* the next field to answer */
static int s_osk_request_open = 0;    /* a person's answer to that field is pending */
/* The slot's holder is active from its InitStart until it is back at NONE: once its own
 * GetStatus has reported the way back, or at once for a savedata ShutdownStart from a caller
 * worse than 0x20 (h_DlgShutdown). Only then may another InitStart take the slot. Caller holds
 * dialog_lock. */
static int utility_slot_active_locked(void) {
    switch (s_utility_slot) {
    case UTILITY_SLOT_SAVEDATA:    return s_dlg_status != 0;
    case UTILITY_SLOT_MSG:         return s_msg_dialog.status != 0;
    case UTILITY_SLOT_NETCONF:     return s_net_dialog.status != 0;
    case UTILITY_SLOT_GAMEDATA:    return s_gamedata_dialog.status != 0;
    case UTILITY_SLOT_GAMESHARING: return s_sharing_dialog.status != 0;
    case UTILITY_SLOT_OSK:         return s_osk_status != OSK_NONE;
    default:                       return 0;
    }
}

/* ---- Scripted answers, for automation. SR_OSK_SCRIPT=<file> supplies one answer per
 * field in order: the line's UTF-8 text, or the keyword CANCEL to answer that field as
 * cancelled. SR_OSK_TEXT=<text> answers every field with the same text (CANCEL cancels
 * every field). While either is set no native box is opened, because an automated route
 * that reaches an unanswered keyboard would block on host UI: a field the script does not
 * cover is answered CANCELLED and the shortfall is named on stderr. With neither set the
 * keyboard is answered by a person, exactly as before. */
#define OSK_SCRIPT_FIELDS 8
#define OSK_SCRIPT_BYTES  255
static char s_osk_answers[OSK_SCRIPT_FIELDS][OSK_SCRIPT_BYTES + 1];
static int s_osk_answer_count;
static const char *s_osk_repeat;                          /* SR_OSK_TEXT */
static int s_osk_script_read;

static int osk_answer_is_cancel(const char *t) {
    return (t[0] | 0x20) == 'c' && (t[1] | 0x20) == 'a' && (t[2] | 0x20) == 'n' &&
           (t[3] | 0x20) == 'c' && (t[4] | 0x20) == 'e' && (t[5] | 0x20) == 'l' && t[6] == '\0';
}

/* The script file is UTF-8; SceUtilityOskData.outtext holds UTF-16. A malformed byte becomes
 * U+FFFD so a bad script shows up in the guest's text instead of vanishing, and a character
 * that needs a surrogate pair is dropped rather than split when one unit of field is left. */
static void osk_decode_utf8(const char *text, wchar_t *out, int max) {
    const unsigned char *p = (const unsigned char *)text;
    int n = 0;
    while (*p && n < max) {
        unsigned cp;
        int extra;
        if (*p < 0x80)                 { cp = *p++; extra = 0; }
        else if ((*p & 0xe0) == 0xc0)  { cp = (unsigned)(*p++ & 0x1f); extra = 1; }
        else if ((*p & 0xf0) == 0xe0)  { cp = (unsigned)(*p++ & 0x0f); extra = 2; }
        else if ((*p & 0xf8) == 0xf0)  { cp = (unsigned)(*p++ & 0x07); extra = 3; }
        else                           { p++; cp = 0xfffdu; extra = 0; }
        while (extra-- > 0) {
            if ((*p & 0xc0) != 0x80) { cp = 0xfffdu; break; }
            cp = (cp << 6) | (unsigned)(*p++ & 0x3f);
        }
        if (cp < 0x10000u) {
            out[n++] = (wchar_t)cp;
        } else if (n + 1 < max) {
            cp -= 0x10000u;
            out[n++] = (wchar_t)(0xd800u + (cp >> 10));
            out[n++] = (wchar_t)(0xdc00u + (cp & 0x3ffu));
        } else {
            break;
        }
    }
    out[n] = 0;
}

static void osk_script_load(void) {
    const char *path = getenv("SR_OSK_SCRIPT");
    s_osk_script_read = 1;
    s_osk_answer_count = 0;
    s_osk_repeat = getenv("SR_OSK_TEXT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "osk: SR_OSK_SCRIPT=%s cannot be read; every field is answered CANCEL\n",
                path);
        return;
    }
    char line[OSK_SCRIPT_BYTES + 2];
    while (s_osk_answer_count < OSK_SCRIPT_FIELDS && fgets(line, (int)sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;               /* blank and comment lines */
        memcpy(s_osk_answers[s_osk_answer_count++], line, n + 1);
    }
    fclose(f);
}

static int osk_script_configured(void) {
    const char *path = getenv("SR_OSK_SCRIPT");
    if (!s_osk_script_read) osk_script_load();
    return (path && *path) || s_osk_repeat;
}

static void osk_read_utf16(uint32_t addr, wchar_t *out, int max) {
    int i = 0;
    if (addr) for (; i < max - 1; i++) {
        uint16_t c = MEM_R16(addr + (uint32_t)i * 2);
        if (!c) break;
        out[i] = (wchar_t)c;
    }
    out[i] = 0;
}

/* Close a person's request the keyboard no longer wants (the title shut the keyboard down,
 * or started a new one, before it was answered); a late answer is discarded. */
void osk_abandon_request(void) {
    if (s_osk_request_open) sr_osk_text_entry_abandon();
    s_osk_request_open = 0;
}

/* Answer the keyboard's fields in order, from the script or from a person. Returns 1 once
 * every field holds its answer, 0 while a person's answer to the current field is pending
 * (that field and the ones after it are left untouched until it arrives). */
static int osk_collect(void) {
    uint32_t p = s_osk_param;
    if (!p) return 1;
    int nf = (int)MEM_R32(p + 0x30);                       /* datacount */
    uint32_t fields = MEM_R32(p + 0x34);                   /* SceUtilityOskData[] */
    if (nf < 1 || nf > 8 || !fields) return 1;
    int scripted = osk_script_configured();
    for (; s_osk_field < nf; s_osk_field++) {
        int i = s_osk_field;
        uint32_t f = fields + (uint32_t)i * 0x34;
        uint32_t descA = MEM_R32(f + 0x1c), inA = MEM_R32(f + 0x20);
        uint32_t outLen = MEM_R32(f + 0x24), outA = MEM_R32(f + 0x28);
        uint32_t outLimit = MEM_R32(f + 0x30);
        wchar_t desc[128], intext[256], out[256];
        osk_read_utf16(descA, desc, 128);
        osk_read_utf16(inA, intext, 256);
        int cap = outLen ? (int)outLen : 256;              /* u16 units incl. terminator */
        if (outLimit && (int)outLimit + 1 < cap) cap = (int)outLimit + 1;
        if (cap > 256) cap = 256;
        wcscpy(out, intext);
        int ok;
        if (!scripted) {
            /* SceUtilityOskData.inputtype sits at +0x10 in the public PSPSDK layout. */
            uint32_t input_type = (uint32_t)MEM_R32(f + 0x10);
            int answer = sr_osk_text_entry_poll(desc, intext, out, cap, input_type);
            if (answer == SR_OSK_TEXT_PENDING) {
                s_osk_request_open = 1;
                return 0;
            }
            s_osk_request_open = 0;
            ok = answer == 1;
        } else {
            const char *ans = i < s_osk_answer_count ? s_osk_answers[i] : s_osk_repeat;
            if (ans) osk_decode_utf8(ans, out, cap - 1);
            ok = ans && !osk_answer_is_cancel(ans);
            if (!ans)
                fprintf(stderr, "osk: no scripted answer for field %d of %d; answered CANCEL\n",
                        i, nf);
        }
        if (outA) {
            const wchar_t *w = ok ? out : intext;
            int j = 0;
            for (; w[j] && j < cap - 1; j++) MEM_W16(outA + (uint32_t)j * 2, (uint16_t)w[j]);
            MEM_W16(outA + (uint32_t)j * 2, 0);
        }
        MEM_W32(f + 0x2c, ok ? 2u : 1u);                   /* result: CHANGED / CANCELLED */
        if (getenv("SR_DLGLOG"))
            fprintf(stderr, "osk: field %d desc='%ls' in='%ls' -> %s '%ls'\n",
                    i, desc, intext, ok ? "ok" : "cancel", ok ? out : intext);
    }
    MEM_W32(p + 0x1c, 0);                                  /* common result */
    return 1;
}

/* A keyboard, like every utility dialog, starts only while the slot's holder is at NONE: a
 * running keyboard or dialog keeps the slot, its open request included (#1386). */
static uint32_t h_OskInitStart(CpuState *s) {
    /* PSPAutotests utility/dialog/sizes: OSK has two versioned sizes. After the busy check
     * (utility_slot_busy), check the size word, then the entire version before retaining the
     * guest address. */
    static const uint32_t sizes[] = { 0x40u, 0x44u };
    if (utility_slot_busy()) return SCE_ERROR_UTILITY_INVALID_STATUS;
    if (!utility_param_size_ok(A0, sizes, sizeof(sizes) / sizeof(sizes[0]))) return 0x80110004u;
    dialog_lock();
    const uint32_t claim = utility_slot_claim_locked(UTILITY_SLOT_OSK);
    if (!claim) {
        s_osk_param = A0;
        s_osk_status = OSK_INIT;
        s_osk_field = 0;
        s_osk_script_read = 0;                             /* a new keyboard re-reads its answers */
    }
    dialog_unlock();
    return claim;
}
/* GetStatus with no OSK ever started (e.g. polled every boot frame while a savedata dialog
 * is up) is SCE_ERROR_UTILITY_WRONG_TYPE; once one has run NONE(0) is a real status. */
static uint32_t h_OskGetStatus(CpuState *s) {
    (void)s;
    if (s_utility_slot != UTILITY_SLOT_OSK) return SCE_ERROR_UTILITY_WRONG_TYPE;
    int ret = s_osk_status;
    if (s_osk_status == OSK_INIT) s_osk_status = OSK_VISIBLE;
    else if (s_osk_status == OSK_VISIBLE) { if (osk_collect()) s_osk_status = OSK_QUIT; }
    else if (s_osk_status == OSK_FINISHED) { s_osk_status = OSK_NONE; s_osk_param = 0; }
    return (uint32_t)ret;                                  /* QUIT stands until ShutdownStart */
}
/* PSP-3000 measurements (#363): no keyboard returns WRONG_TYPE (0x80110005);
 * INIT, VISIBLE and NONE after use refuse shutdown with INVALID_STATUS (0x80110001).
 * QUIT accepts shutdown; GetStatus keeps the existing wind-down to NONE. */
static uint32_t h_OskShutdown(CpuState *s) {
    (void)s;
    if (s_utility_slot != UTILITY_SLOT_OSK) return SCE_ERROR_UTILITY_WRONG_TYPE;
    if (s_osk_status == OSK_NONE || s_osk_status == OSK_INIT || s_osk_status == OSK_VISIBLE)
        return 0x80110001u;
    osk_abandon_request();
    if (s_osk_status) s_osk_status = OSK_FINISHED;         /* then NONE on the next poll */
    return 0;
}
/* PSP-3000 measurements (#363): Update succeeds in VISIBLE (speed 0 and 1),
 * returns WRONG_TYPE before a keyboard starts, and INVALID_STATUS in INIT, QUIT and NONE after use. */
static uint32_t h_OskUpdate(CpuState *s) {
    (void)s;
    if (s_utility_slot != UTILITY_SLOT_OSK) return SCE_ERROR_UTILITY_WRONG_TYPE;
    if (s_osk_status == OSK_NONE || s_osk_status == OSK_INIT || s_osk_status == OSK_QUIT)
        return 0x80110001u;
    return 0;
}

/* Single definition, called by both registry branches, so the executable HLE harness
 * dispatches the same OSK mapping the game build does instead of carrying a second,
 * drifting test mapping. */
void hle_register_osk_handlers(void) {
    sr_hle_register(0xf3f76017, "sceUtilityOskGetStatus", h_OskGetStatus);
    sr_hle_register(0x3dfaeba9, "sceUtilityOskShutdownStart", h_OskShutdown);
    /* 0xf6269b82 is OskInitStart, NOT GetSystemParamString -- the old string handler wrote
     * A2 bytes through A1, both garbage for this signature. */
    sr_hle_register(0xf6269b82, "sceUtilityOskInitStart", h_OskInitStart);
}

/* The other utility dialogs share the OSK's slot (#1386); one definition for both registry
 * branches, so the executable HLE harness dispatches the mapping the game build does.
 * sceUtilitySavedataInitStart is registered with hle_register_partition_savedata_handlers(). */
void hle_register_utility_dialog_handlers(void) {
    sr_hle_register(0x9790b33c, "sceUtilitySavedataShutdownStart", h_DlgShutdown);
    sr_hle_register(0x8874dbe0, "sceUtilitySavedataGetStatus", h_DlgGetStatus);
    sr_hle_register(0xd4b95ffb, "sceUtilitySavedataUpdate", h_SavedataUpdate);
    sr_hle_register(0x2ad8e239, "sceUtilityMsgDialogInitStart", h_MsgDialogInit);
    sr_hle_register(0x95fc253b, "sceUtilityMsgDialogUpdate", h_MsgDialogUpdate);
    sr_hle_register(0x9a1c91d7, "sceUtilityMsgDialogGetStatus", h_MsgDialogStatus);
    sr_hle_register(0x67af3428, "sceUtilityMsgDialogShutdownStart", h_MsgDialogShutdown);
    sr_hle_register(0x4928bd96, "sceUtilityMsgDialogAbort", h_MsgDialogAbort);
    sr_hle_register(0x4db1e739, "sceUtilityNetconfInitStart", h_NetDialogInit);
    sr_hle_register(0x91e70e35, "sceUtilityNetconfUpdate", h_NetDialogUpdate);
    sr_hle_register(0x6332aa39, "sceUtilityNetconfGetStatus", h_NetDialogStatus);
    sr_hle_register(0xf88155f6, "sceUtilityNetconfShutdownStart", h_NetDialogShutdown);
    sr_hle_register(0x24ac31eb, "sceUtilityGamedataInstallInitStart", h_GamedataDialogInit);
    sr_hle_register(0x4aecd179, "sceUtilityGamedataInstallUpdate", h_GamedataDialogUpdate);
    sr_hle_register(0xb57e95d9, "sceUtilityGamedataInstallGetStatus", h_GamedataDialogStatus);
    sr_hle_register(0x32e32dcb, "sceUtilityGamedataInstallShutdownStart", h_GamedataDialogShutdown);
    sr_hle_register(0xc492f751, "sceUtilityGameSharingInitStart", h_SharingDialogInit);
    sr_hle_register(0x7853182d, "sceUtilityGameSharingUpdate", h_SharingDialogUpdate);
    sr_hle_register(0x946963f3, "sceUtilityGameSharingGetStatus", h_SharingDialogStatus);
    sr_hle_register(0xefc6f80f, "sceUtilityGameSharingShutdownStart", h_SharingDialogShutdown);
}

void hle_register_utility_module_handlers(void) {
    sr_hle_register(0xc629af26, "sceUtilityLoadAvModule", h_UtilityLoadAvModule);
    sr_hle_register(0xf7d8d092, "sceUtilityUnloadAvModule", h_UtilityUnloadAvModule);
    sr_hle_register(0x2a2b3de0, "sceUtilityLoadModule", h_UtilityLoadModule);
    sr_hle_register(0xe49bfe92, "sceUtilityUnloadModule", h_UtilityUnloadModule);
    sr_hle_register(0x1579a159, "sceUtilityLoadNetModule", h_UtilityLoadNetModule);
    sr_hle_register(0x64d50c56, "sceUtilityUnloadNetModule", h_UtilityUnloadNetModule);
    sr_hle_register(0x0d5bc6d2, "sceUtilityLoadUsbModule", h_UtilityLoadUsbModule);
    sr_hle_register(0xf64910f0, "sceUtilityUnloadUsbModule", h_UtilityUnloadUsbModule);
    sr_hle_register(0x4b85c861, "sceUtilityOskUpdate", h_OskUpdate);
}

/* sceImpose language/confirm-button mode (see h_ImposeSetLanguageMode). One definition, called
 * by both registry branches, so the executable harness dispatches the production mapping. */
void hle_register_impose_handlers(void) {
    sr_hle_register(0x36aa6e91, "sceImposeSetLanguageMode", h_ImposeSetLanguageMode);
    sr_hle_register(0x24fd7bcf, "sceImposeGetLanguageMode", h_ImposeGetLanguageMode);
}

void sr_hle_loadexec_reset_guest_state(void) {
    /* The replacement program is a module record of its own; the old main module's record
     * and module-start thread parameters no longer apply. */
    s_main_module_record = NULL;
    sched_set_module_start_thread(SCE_KERNEL_MODULE_INIT_PRIORITY, 0u, 0u, NULL, 0u);
    if (s_loadexec_request.args) free(s_loadexec_request.args);
    memset(&s_loadexec_request, 0, sizeof(s_loadexec_request));
    s_loadexec_pending = 0;
    /* Retire module code/export authority before resetting the allocator and RAM. */
    while (s_prx_image_count != 0u)
        unload_prx_image(s_prx_images[0].base);
    memset(s_loaded_modules, 0, sizeof(s_loaded_modules));
    s_nloaded_modules = 0;
    s_deferred_self_stop_uid = 0u;
    s_deferred_self_stop_thread_uid = 0u;
    s_last_prx_base = s_last_prx_entry = s_last_prx_stop = 0u;
    s_prx_stage_base = s_prx_stage_len = 0u;
    s_prx_staging = 0;
    s_psmfplayer_lib_version = 0u;
    atomic_store_explicit(&s_prx_started_count, 0, memory_order_release);
    loadexec_reset_user_partition();

    for (unsigned i = 0; i < FPL_MAX; i++) {
        free(s_fpls[i].free_blocks);
        free(s_fpls[i].allocated);
    }
    memset(s_fpls, 0, sizeof(s_fpls));
    for (unsigned i = 0; i < VPL_MAX; i++) vpl_release(&s_vpls[i]);
    memset(s_sync, 0, sizeof(s_sync));
    memset(s_mbx, 0, sizeof(s_mbx));
    mutex_reset_all();

    if (s_callbacks) free(s_callbacks);
    s_callbacks = NULL;
    s_callbacks_len = 0u;
    s_callbacks_cap = 0u;
    memset(s_memstick_callbacks, 0, sizeof(s_memstick_callbacks));
    s_memstick_callback_count = 0u;
    s_exit_cb_uid = 0u;
    s_exit_request_pending = 0;
    s_exit_request_deadline_us = 0u;
    memset(s_utility_module_loaded, 0, sizeof(s_utility_module_loaded));
    s_utility_usb_loaded = 0u;
    s_volatile_locked = 0;
    g_sdk_version = 0u;
    s_loadexec_boot_uid = 0u;
    s_main_module_uid = 0u;
    sr_hle_power_loadexec_reset();

    hle_fd_init();
    memset(s_io_cwds, 0, sizeof(s_io_cwds));

    /* Media-player state owns host decoder/producer objects as well as guest
     * pointers. Close those objects before the next program reuses guest RAM. */
    for (size_t i = 0; i < sizeof(s_psmf_players) / sizeof(s_psmf_players[0]); i++) {
        psmf_player_release(&s_psmf_players[i]);
    }
    for (unsigned i = 0; i < sizeof(s_atrac) / sizeof(s_atrac[0]); i++) {
        atrac_release_decoder(&s_atrac[i]);
        atrac_stream_window_free(&s_atrac[i]);
        memset(&s_atrac[i], 0, sizeof(s_atrac[i]));
    }
    s_atrac_caps_configured = 0;
    memset(s_atrac_slot_codec, 0, sizeof(s_atrac_slot_codec));
    hle_audio_guest_reset();
    hle_sas_guest_reset();

    osk_abandon_request();
    s_osk_status = OSK_NONE;
    s_osk_param = 0u;
    s_osk_field = 0;
    s_osk_request_open = 0;
    s_osk_answer_count = 0;
    s_osk_repeat = NULL;
    s_osk_script_read = 0;
    memset(s_osk_answers, 0, sizeof(s_osk_answers));
    dialog_lock();
    utility_dialogs_reset_locked();
    dialog_unlock();

    s_uid = 0x110u;
    fprintf(stderr, "LOAD_EXEC_GUEST_RESET_SYNTHETIC: memory, scheduler objects, callbacks, "
                    "module records and file descriptors will be rebuilt for the next program\n");
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_partition_savedata_handlers(). */
void hle_register_partition_savedata_utility(void) {
    sr_hle_register(0x50c4cd57, "sceUtilitySavedataInitStart", h_SavedataInitStart);
}

/* From hle_register_sysreg_handlers(). */
void hle_register_sysreg_utility(void) {
    sr_hle_register(0xa5da2406, "sceUtilityGetSystemParamInt", h_GetSystemParamInt);
    sr_hle_register(0x34b78343, "sceUtilityGetSystemParamString", h_GetSystemParamString);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_utility(void) {
    sr_hle_register_unsupported(0x72189c48u, "sceImposeSetUMDPopup", 0x80010086u);
    sr_hle_register_unsupported(0x8c943191u, "sceImposeGetBatteryIconStatus", 0x80010086u);
}
