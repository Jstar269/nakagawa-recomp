// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* NP DRM: the sceNpDrm licensee key. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* sceNpDrmSetLicenseeKey(const u8 key[16]) records the title's 16-byte licensee key for a later
 * sceNpDrmEdataSetupKey and returns 0; sceNpDrmClearLicenseeKey(void) forgets it and returns 0.
 * Recording the key is the whole contract of both calls: no key material ships with this project
 * and nothing here decrypts, so EdataSetupKey stays refused below. An unreadable key pointer
 * returns SCE_NPDRM_ERROR_INVALID_FILE (0x80550902), PPSSPP's choice, not measured. */
#define SR_NPDRM_LICENSEE_KEY_BYTES 16u
static uint8_t s_npdrm_licensee_key[SR_NPDRM_LICENSEE_KEY_BYTES];
static uint32_t s_npdrm_licensee_key_set = 0u;
static uint32_t h_NpDrmSetLicenseeKey(CpuState *s) {
    if (!A0 || !sr_guest_span_readable(A0, SR_NPDRM_LICENSEE_KEY_BYTES)) return 0x80550902u;
    for (uint32_t i = 0; i < SR_NPDRM_LICENSEE_KEY_BYTES; i++) s_npdrm_licensee_key[i] = MEM_R8(A0 + i);
    s_npdrm_licensee_key_set = 1u;
    return 0;
}
static uint32_t h_NpDrmClearLicenseeKey(CpuState *s) {
    (void)s;
    memset(s_npdrm_licensee_key, 0, sizeof(s_npdrm_licensee_key));
    s_npdrm_licensee_key_set = 0u;
    return 0;
}
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_npdrm_licensee_key(uint8_t out[16]) {
    memcpy(out, s_npdrm_licensee_key, sizeof(s_npdrm_licensee_key));
    return s_npdrm_licensee_key_set;
}
#endif

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_kernel_module_handlers(). */
void hle_register_kernel_module_npdrm(void) {
    sr_hle_register(0xa1336091u, "sceNpDrmSetLicenseeKey", h_NpDrmSetLicenseeKey);
    sr_hle_register(0x9b745542u, "sceNpDrmClearLicenseeKey", h_NpDrmClearLicenseeKey);
    /* No NP DRM key material is part of this project, so the edata key setup is refused. The
     * module-from-buffer loader is not implemented, so its USB/WLAN variant is refused too. Both use
     * SCE_KERNEL_ERROR_NOTIMP, the runtime's controlled refusal for kernel functions it does not provide. */
    sr_hle_register_unsupported(0x08d98894u, "sceNpDrmEdataSetupKey", 0x80020002u);
}
