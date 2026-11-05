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

/* ---- The sceNet core library with no network (#341). Nakagawa has no network stack and opens no
 * socket (AGENTS.md section 5), but on a PSP the core library's start-up needs no network either:
 * sceNetInit only takes its memory pool and starts its threads, and the console says "no network"
 * later, when a game asks for an address, a group or an access point. pspautotests records that on a
 * PSP (hrydgard/pspautotests 70e72391, tests/net): sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000)
 * returns 0 in every net test, sceNetTerm returns 0, and Term followed by Init again returns 0
 * (net/adhoc/socketids). A game that treats a refused sceNetInit as impossible goes on with a
 * network object it never set up: LittleBigPlanet's "connect" jumped through a NULL method there.
 *
 * So sceNetInit and sceNetTerm keep the library's state and its pool, and the calls that need a
 * network answer as a PSP with no group and no access point connection does. Nothing here reaches a
 * host network API. UNMEASURED, from PPSSPP Core/HLE/sceNet.cpp (193fd934, a behaviour reference):
 * the argument checks, the pool taken from the user partition, a second sceNetInit without Term
 * (PPSSPP starts over; the PSP is said to leak the first pool), and sceNetTerm before sceNetInit
 * (0). The library's two threads (SceNetCallout, SceNetNetintr) are not created. */
static struct {
    int inited;
    uint32_t pool_uid;  /* the user-partition block holding the pool */
} s_netlib;

#define NET_POOL_NONE 0xffffffffu

/* sceNetInit(poolSize, calloutPri, calloutStack, netintrPri, netintrStack). */
static uint32_t h_NetInit(CpuState *s) {
    const uint32_t pool_size = A0, callout_pri = A1, netintr_pri = A3;
    if (pool_size == 0u) return 0x800201b7u;               /* SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE */
    if (callout_pri < 0x08u || callout_pri > 0x77u || netintr_pri < 0x08u || netintr_pri > 0x77u)
        return SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
    if (s_netlib.inited && s_netlib.pool_uid != NET_POOL_NONE) free_block(s_netlib.pool_uid);
    s_netlib.inited = 0;
    s_netlib.pool_uid = alloc_block(pool_size);
    if (s_netlib.pool_uid == NET_POOL_NONE) return SCE_KERNEL_ERROR_NO_MEMORY;
    s_netlib.inited = 1;
    return 0u;
}

/* sceNetTerm(void): frees the pool; 0 (measured after sceNetInit). */
static uint32_t h_NetTerm(CpuState *s) {
    (void)s;
    if (s_netlib.inited && s_netlib.pool_uid != NET_POOL_NONE) free_block(s_netlib.pool_uid);
    s_netlib.inited = 0;
    s_netlib.pool_uid = NET_POOL_NONE;
    return 0u;
}

/* sceNetGetLocalEtherAddr(u8 *mac): the address exists only once an ad hoc group operation or an
 * access-point connection has started. Before that a PSP returns 0x80410180 and leaves the buffer
 * untouched (pspautotests net/adhoc/apctlinit, before, during and after sceNetApctlInit; also
 * net/adhoc/adhocctl and net/adhoc/pdp before a group). With no network neither ever starts here. */
#define SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE 0x80410180u
static uint32_t h_NetGetLocalEtherAddr(CpuState *s) {
    (void)s;
    return SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE;
}

/* sceNetAdhocctlGetParameter(SceNetAdhocctlParameter *param): sceNetAdhocctlInit stays refused
 * (no network), so the ad hoc control library is never initialized and the call returns its
 * NOT_INITIALIZED code, 0x80410b08, writing nothing. A PSP returns that code before
 * sceNetAdhocctlInit to GetState, AddHandler and Create (pspautotests net/adhoc/adhocctl
 * "Before init"); GetParameter's own pre-init answer is UNMEASURED and follows them, as PPSSPP's
 * order does with WLAN on. After init and out of a group the PSP answers 0x80410b09. */
#define SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED 0x80410b08u
static uint32_t h_NetAdhocctlGetParameter(CpuState *s) {
    (void)s;
    return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
}

void sr_hle_test_net_reset(void) {
    s_netlib.inited = 0;
    s_netlib.pool_uid = NET_POOL_NONE;
}
int sr_hle_test_net_inited(void) { return s_netlib.inited; }
uint32_t sr_hle_test_net_pool_uid(void) { return s_netlib.pool_uid; }

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
    /* The core library with no network (h_NetInit): it starts and stops as on a PSP, and the
     * local address is unavailable because no group or access point ever connects. */
    sr_hle_register(0x0bf0a3aeu, "sceNetGetLocalEtherAddr", h_NetGetLocalEtherAddr);
    sr_hle_register(0x89360950u, "sceNetEtherNtostr", h_NetEtherNtostr);
    sr_hle_register(0x39af39a6u, "sceNetInit", h_NetInit);
    sr_hle_register(0x281928a9u, "sceNetTerm", h_NetTerm);
    /* The net stack's thread and memory bookkeeping, and ad hoc discovery, which has no peers to
     * find, stay refused with the offline network code 0x80010086 (SCE_KERNEL_ERROR_ERRNO_
     * FUNCTION_NOT_SUPPORTED): their answers with no network are not modelled yet. */
    sr_hle_register_unsupported(0x50647530u, "sceNetFreeThreadinfo", 0x80010086u);
    sr_hle_register_unsupported(0xcc393e48u, "sceNetGetMallocStat", 0x80010086u);
    sr_hle_register_unsupported(0xf7d13214u, "sceNetAdhocDiscoverStop", 0x80010086u);
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
     * SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED (0x80010086). The code is not a measured ad
     * hoc result; PSP-visible ad hoc state and errors are unmeasured. sceNetAdhocctlGetParameter
     * is the exception: it answers that the library is not initialized (h_NetAdhocctlGetParameter). */
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
    sr_hle_register(0xded9d28eu, "sceNetAdhocctlGetParameter", h_NetAdhocctlGetParameter);
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
