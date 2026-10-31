// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Networking: sceNet, sceHttp, sceSsl and sceWlan, refused or answered without a network.
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* sceWlanGetEtherAddr(u8 *addr): the 6-byte MAC address of the console profile. The whole
 * output span must be guest memory; NULL or an unwritable span answers ILLEGAL_ADDR and writes
 * nothing (the PSP's answer is not measured: the console's own address is never read). */
static uint32_t h_WlanGetEtherAddr(CpuState *s) {
    if (!A0 || !sr_guest_span_writable(A0, 6u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!console_profile_identity())
        return hle_unmodelled_stop("sceWlanGetEtherAddr", "has no address: the host's random "
                                   "source is unavailable");
    for (int i = 0; i < 6; i++) MEM_W8(A0 + (uint32_t)i, s_console.mac[i]);
    return 0;
}

/* sceWlanGetSwitchState and sceWlanDevIsPowerOn: the console profile's WLAN switch, 1 (on) unless
 * the overlay's console object sets "wlan_switch": 0. Both answer 1 on the PSP-3000 6.61 oracle
 * with its switch on (PSP-KERNEL-SEM-001 wlan-state, 2026-10-31); what DevIsPowerOn answers with
 * the switch off is UNMEASURED, so it follows the switch. */
static uint32_t h_WlanOn(CpuState *s) {
    (void)s;
    sreg_ensure_loaded();
    return s_console.wlan_switch_set ? s_console.wlan_switch : 1u;
}

/* sceNetEtherNtostr(const unsigned char *mac, char *name): format the six-byte
 * hardware address locally. This does not require a host network stack. */
static uint32_t h_NetEtherNtostr(CpuState *s) {
    (void)s;
    uint32_t mac = A0, out = A1;
    if (!mac || !out || !sr_guest_span_readable(mac, 6u) ||
        !sr_guest_span_writable(out, 18u))
        return 0u;
    char text[18];
    snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x",
             MEM_R8(mac), MEM_R8(mac + 1u), MEM_R8(mac + 2u),
             MEM_R8(mac + 3u), MEM_R8(mac + 4u), MEM_R8(mac + 5u));
    for (uint32_t i = 0; i < sizeof text; i++)
        MEM_W8(out + i, (uint8_t)text[i]);
    return out;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_console_profile_handlers(). */
void hle_register_console_profile_network(void) {
    sr_hle_register(0x0c622081, "sceWlanGetEtherAddr", h_WlanGetEtherAddr);
    sr_hle_register(0x93440b11, "sceWlanDevIsPowerOn", h_WlanOn);
    sr_hle_register(0xd7763699, "sceWlanGetSwitchState", h_WlanOn);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_network_1(void) {
    sr_hle_register_unsupported(0x0bf0a3aeu, "sceNetGetLocalEtherAddr", 0x80010086u);
    sr_hle_register(0x89360950u, "sceNetEtherNtostr", h_NetEtherNtostr);
    /* sceNetInit follows the same offline network policy as sceNetGetLocalEtherAddr: the runtime
     * has no network stack, so the call refuses with that code rather than reporting success. */
    sr_hle_register_unsupported(0x39af39a6u, "sceNetInit", 0x80010086u);
    /* A title that sees sceNetInit fail may still execute its cleanup path. Keep that
     * cleanup as a named offline refusal instead of the unknown-NID stop. */
    sr_hle_register_unsupported(0x281928a9u, "sceNetTerm", 0x80010086u);
    /* sceHttpsEnd follows the offline HTTP policy of sceHttpEnd and sceHttpInit: no network stack. */
    sr_hle_register_unsupported(0xf9d8eb63u, "sceHttpsEnd", 0x80010086u);
    /* The SSL library has no handler in this runtime, which has no network stack. sceSslEnd follows
     * the same offline policy as the HTTP family (0x80010086); the other SSL calls are not modelled. */
    sr_hle_register_unsupported(0x191cdeffu, "sceSslEnd", 0x80010086u);
    /* sceNetApctl is the access-point control library; with no network stack the runtime refuses its
     * handler calls under the offline network policy (0x80010086). */
    sr_hle_register_unsupported(0x5963991bu, "sceNetApctlDelHandler", 0x80010086u);
    /* The public ad hoc libraries (#1139 phase 0) sit above sceNet: the PSPSDK prototypes in
     * pspnet_adhoc.h (22), pspnet_adhocctl.h (21) and pspnet_adhocmatching.h (16), each NID
     * sha1(name) as tools/nid_corpus.json records for its pspsdk-sourced names. With no network
     * stack they refuse under the offline network policy, with the runtime's documented
     * SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED that sceNetInit returns. The code is not a
     * measured ad hoc result; PSP-visible ad hoc state and errors are unmeasured. */
    sr_hle_register_unsupported(0x7f75c338u, "sceNetAdhocGameModeCreateMaster", 0x80010086u);
    sr_hle_register_unsupported(0x3278ab0cu, "sceNetAdhocGameModeCreateReplica", 0x80010086u);
    sr_hle_register_unsupported(0xa0229362u, "sceNetAdhocGameModeDeleteMaster", 0x80010086u);
    sr_hle_register_unsupported(0x0b2228e9u, "sceNetAdhocGameModeDeleteReplica", 0x80010086u);
    sr_hle_register_unsupported(0x98c204c8u, "sceNetAdhocGameModeUpdateMaster", 0x80010086u);
    sr_hle_register_unsupported(0xfa324b4eu, "sceNetAdhocGameModeUpdateReplica", 0x80010086u);
    sr_hle_register_unsupported(0xc7c1fc57u, "sceNetAdhocGetPdpStat", 0x80010086u);
    sr_hle_register_unsupported(0xb9685118u, "sceNetAdhocGetPtpStat", 0x80010086u);
    sr_hle_register_unsupported(0xe1d621d7u, "sceNetAdhocInit", 0x80010086u);
    sr_hle_register_unsupported(0x6f92741bu, "sceNetAdhocPdpCreate", 0x80010086u);
    sr_hle_register_unsupported(0x7f27bb5eu, "sceNetAdhocPdpDelete", 0x80010086u);
    sr_hle_register_unsupported(0xdfe53e03u, "sceNetAdhocPdpRecv", 0x80010086u);
    sr_hle_register_unsupported(0xabed3790u, "sceNetAdhocPdpSend", 0x80010086u);
    sr_hle_register_unsupported(0x9df81198u, "sceNetAdhocPtpAccept", 0x80010086u);
    sr_hle_register_unsupported(0x157e6225u, "sceNetAdhocPtpClose", 0x80010086u);
    sr_hle_register_unsupported(0xfc6fc07bu, "sceNetAdhocPtpConnect", 0x80010086u);
    sr_hle_register_unsupported(0x9ac2eeacu, "sceNetAdhocPtpFlush", 0x80010086u);
    sr_hle_register_unsupported(0xe08bdac1u, "sceNetAdhocPtpListen", 0x80010086u);
    sr_hle_register_unsupported(0x877f6d66u, "sceNetAdhocPtpOpen", 0x80010086u);
    sr_hle_register_unsupported(0x8bea2b3eu, "sceNetAdhocPtpRecv", 0x80010086u);
    sr_hle_register_unsupported(0x4da4c788u, "sceNetAdhocPtpSend", 0x80010086u);
    sr_hle_register_unsupported(0xa62c6f57u, "sceNetAdhocTerm", 0x80010086u);
    sr_hle_register_unsupported(0x20b317a0u, "sceNetAdhocctlAddHandler", 0x80010086u);
    sr_hle_register_unsupported(0x0ad043edu, "sceNetAdhocctlConnect", 0x80010086u);
    sr_hle_register_unsupported(0xec0635c1u, "sceNetAdhocctlCreate", 0x80010086u);
    sr_hle_register_unsupported(0xa5c055ceu, "sceNetAdhocctlCreateEnterGameMode", 0x80010086u);
    sr_hle_register_unsupported(0x6402490bu, "sceNetAdhocctlDelHandler", 0x80010086u);
    sr_hle_register_unsupported(0x34401d65u, "sceNetAdhocctlDisconnect", 0x80010086u);
    sr_hle_register_unsupported(0xcf8e084du, "sceNetAdhocctlExitGameMode", 0x80010086u);
    sr_hle_register_unsupported(0x99560abeu, "sceNetAdhocctlGetAddrByName", 0x80010086u);
    sr_hle_register_unsupported(0x362cbe8fu, "sceNetAdhocctlGetAdhocId", 0x80010086u);
    sr_hle_register_unsupported(0x5a014ce0u, "sceNetAdhocctlGetGameModeInfo", 0x80010086u);
    sr_hle_register_unsupported(0x8916c003u, "sceNetAdhocctlGetNameByAddr", 0x80010086u);
    sr_hle_register_unsupported(0xded9d28eu, "sceNetAdhocctlGetParameter", 0x80010086u);
    sr_hle_register_unsupported(0x8db83fdcu, "sceNetAdhocctlGetPeerInfo", 0x80010086u);
    sr_hle_register_unsupported(0xe162cb14u, "sceNetAdhocctlGetPeerList", 0x80010086u);
    sr_hle_register_unsupported(0x81aee1beu, "sceNetAdhocctlGetScanInfo", 0x80010086u);
    sr_hle_register_unsupported(0x75ecd386u, "sceNetAdhocctlGetState", 0x80010086u);
    sr_hle_register_unsupported(0xe26f226eu, "sceNetAdhocctlInit", 0x80010086u);
    sr_hle_register_unsupported(0x5e7f79c9u, "sceNetAdhocctlJoin", 0x80010086u);
    sr_hle_register_unsupported(0x1ff89745u, "sceNetAdhocctlJoinEnterGameMode", 0x80010086u);
    sr_hle_register_unsupported(0x08fff7a0u, "sceNetAdhocctlScan", 0x80010086u);
    sr_hle_register_unsupported(0x9d689e13u, "sceNetAdhocctlTerm", 0x80010086u);
    sr_hle_register_unsupported(0xec19337du, "sceNetAdhocMatchingAbortSendData", 0x80010086u);
    sr_hle_register_unsupported(0xea3c6108u, "sceNetAdhocMatchingCancelTarget", 0x80010086u);
    sr_hle_register_unsupported(0x8f58bedfu, "sceNetAdhocMatchingCancelTargetWithOpt", 0x80010086u);
    sr_hle_register_unsupported(0xca5eda6fu, "sceNetAdhocMatchingCreate", 0x80010086u);
    sr_hle_register_unsupported(0xf16eaf4fu, "sceNetAdhocMatchingDelete", 0x80010086u);
    sr_hle_register_unsupported(0xb5d96c2au, "sceNetAdhocMatchingGetHelloOpt", 0x80010086u);
    sr_hle_register_unsupported(0xc58bcd9eu, "sceNetAdhocMatchingGetMembers", 0x80010086u);
    sr_hle_register_unsupported(0x40f8f435u, "sceNetAdhocMatchingGetPoolMaxAlloc", 0x80010086u);
    sr_hle_register_unsupported(0x9c5cfb7du, "sceNetAdhocMatchingGetPoolStat", 0x80010086u);
    sr_hle_register_unsupported(0x2a2a1e07u, "sceNetAdhocMatchingInit", 0x80010086u);
    sr_hle_register_unsupported(0x5e3d4b79u, "sceNetAdhocMatchingSelectTarget", 0x80010086u);
    sr_hle_register_unsupported(0xf79472d7u, "sceNetAdhocMatchingSendData", 0x80010086u);
    sr_hle_register_unsupported(0xb58e61b7u, "sceNetAdhocMatchingSetHelloOpt", 0x80010086u);
    sr_hle_register_unsupported(0x93ef3843u, "sceNetAdhocMatchingStart", 0x80010086u);
    sr_hle_register_unsupported(0x32b156b3u, "sceNetAdhocMatchingStop", 0x80010086u);
    sr_hle_register_unsupported(0x7945ecdau, "sceNetAdhocMatchingTerm", 0x80010086u);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_network_2(void) {
    sr_hle_register_unsupported(0x0282a3bdu, "sceHttpGetContentLength", 0x80010086u);
    sr_hle_register_unsupported(0x03d9526fu, "sceHttpSetResolveRetry", 0x80010086u);
    sr_hle_register_unsupported(0x1f0fc3e3u, "sceHttpSetRecvTimeOut", 0x80010086u);
    sr_hle_register_unsupported(0x2255551eu, "sceHttpGetNetworkPspError", 0x80010086u);
    sr_hle_register_unsupported(0x3eaba285u, "sceHttpAddExtraHeader", 0x80010086u);
    sr_hle_register_unsupported(0xab1abe07u, "sceHttpInit", 0x80010086u);
    sr_hle_register_unsupported(0xd1c8945eu, "sceHttpEnd", 0x80010086u);
}
