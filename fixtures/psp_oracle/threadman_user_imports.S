/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Complete ThreadManForUser import block for the PSP-B1 and PSP-B2 kernel-object
   probes (psp_b1.c, psp_b2.c).

   PSPSDK ships heavyweight-mutex stubs only for ThreadManForKernel, so these
   user-mode probes declare their own ThreadManForUser block. IMPORT_START emits
   a global __stub_module_ThreadManForUser, which stops the linker from pulling
   the SDK's header member for that library; any SDK ThreadManForUser function
   stub that is still pulled would then land in a second, non-adjacent stub run
   that psp-fixup-imports cannot describe. Declaring every ThreadManForUser
   function the probes and the PSPSDK CRT reference here keeps the library in
   one contiguous run. If a probe starts using a function missing from this
   list, add its NID here.

   Every NID was read from the PSPSDK libpspuser.a `.rodata.sceNid` members and
   matches src/rt/nid_names.h. */

#include "pspimport.s"

    IMPORT_START "ThreadManForUser", 0x40010000

    IMPORT_FUNC  "ThreadManForUser", 0xB7D098C6, sceKernelCreateMutex
    IMPORT_FUNC  "ThreadManForUser", 0xF8170FBE, sceKernelDeleteMutex
    IMPORT_FUNC  "ThreadManForUser", 0xB011B11F, sceKernelLockMutex
    IMPORT_FUNC  "ThreadManForUser", 0x5BF4DD27, sceKernelLockMutexCB
    IMPORT_FUNC  "ThreadManForUser", 0x0DDCD2C9, sceKernelTryLockMutex
    IMPORT_FUNC  "ThreadManForUser", 0x6B30100F, sceKernelUnlockMutex
    IMPORT_FUNC  "ThreadManForUser", 0x87D9223C, sceKernelCancelMutex
    IMPORT_FUNC  "ThreadManForUser", 0xA9C2CB9A, sceKernelReferMutexStatus
    IMPORT_FUNC  "ThreadManForUser", 0x349D6D6C, sceKernelCheckCallback
    IMPORT_FUNC  "ThreadManForUser", 0xE81CAF8F, sceKernelCreateCallback
    IMPORT_FUNC  "ThreadManForUser", 0x19CFF145, sceKernelCreateLwMutex
    IMPORT_FUNC  "ThreadManForUser", 0xD6DA4BA1, sceKernelCreateSema
    IMPORT_FUNC  "ThreadManForUser", 0x446D8DE6, sceKernelCreateThread
    IMPORT_FUNC  "ThreadManForUser", 0xCEADEB47, sceKernelDelayThread
    IMPORT_FUNC  "ThreadManForUser", 0xEDBA5844, sceKernelDeleteCallback
    IMPORT_FUNC  "ThreadManForUser", 0x60107536, sceKernelDeleteLwMutex
    IMPORT_FUNC  "ThreadManForUser", 0xF0B7DA1C, sceKernelDeleteMsgPipe
    IMPORT_FUNC  "ThreadManForUser", 0x28B6489C, sceKernelDeleteSema
    IMPORT_FUNC  "ThreadManForUser", 0x9FA03CD3, sceKernelDeleteThread
    IMPORT_FUNC  "ThreadManForUser", 0x809CE29B, sceKernelExitDeleteThread
    IMPORT_FUNC  "ThreadManForUser", 0xAA73C935, sceKernelExitThread
    IMPORT_FUNC  "ThreadManForUser", 0x293B45B8, sceKernelGetThreadId
    IMPORT_FUNC  "ThreadManForUser", 0xC11BA8C4, sceKernelNotifyCallback
    IMPORT_FUNC  "ThreadManForUser", 0x74829B76, sceKernelReceiveMsgPipe
    IMPORT_FUNC  "ThreadManForUser", 0x33BE4024, sceKernelReferMsgPipeStatus
    IMPORT_FUNC  "ThreadManForUser", 0x17C1684E, sceKernelReferThreadStatus
    IMPORT_FUNC  "ThreadManForUser", 0x876DBFAD, sceKernelSendMsgPipe
    IMPORT_FUNC  "ThreadManForUser", 0x3F53E640, sceKernelSignalSema
    IMPORT_FUNC  "ThreadManForUser", 0xF475845D, sceKernelStartThread
    IMPORT_FUNC  "ThreadManForUser", 0xDF52098F, sceKernelTryReceiveMsgPipe
    IMPORT_FUNC  "ThreadManForUser", 0x884C9F90, sceKernelTrySendMsgPipe
    IMPORT_FUNC  "ThreadManForUser", 0x4E3A1105, sceKernelWaitSema
    IMPORT_FUNC  "ThreadManForUser", 0x278C0DF5, sceKernelWaitThreadEnd
    IMPORT_FUNC "ThreadManForUser", 0x2A3D44FF, sceKernelGetCallbackCount
    IMPORT_FUNC "ThreadManForUser", 0x58B1F937, sceKernelPollSema
    IMPORT_FUNC "ThreadManForUser", 0xBC6FEBC5, sceKernelReferSemaStatus
    IMPORT_FUNC "ThreadManForUser", 0x55C20A00, sceKernelCreateEventFlag
    IMPORT_FUNC "ThreadManForUser", 0x1FB15A32, sceKernelSetEventFlag
    IMPORT_FUNC "ThreadManForUser", 0x812346E4, sceKernelClearEventFlag
    IMPORT_FUNC "ThreadManForUser", 0x30FD48F0, sceKernelPollEventFlag
    IMPORT_FUNC "ThreadManForUser", 0xA66B0120, sceKernelReferEventFlagStatus
    IMPORT_FUNC "ThreadManForUser", 0xEF9E4C70, sceKernelDeleteEventFlag
    IMPORT_FUNC "ThreadManForUser", 0xC07BB470, sceKernelCreateFpl
    IMPORT_FUNC "ThreadManForUser", 0xD979E9BF, sceKernelAllocateFpl
    IMPORT_FUNC "ThreadManForUser", 0x623AE665, sceKernelTryAllocateFpl
    IMPORT_FUNC "ThreadManForUser", 0xF6414A71, sceKernelFreeFpl
    IMPORT_FUNC "ThreadManForUser", 0xD8199E4C, sceKernelReferFplStatus
    IMPORT_FUNC "ThreadManForUser", 0xED1410E0, sceKernelDeleteFpl
    IMPORT_FUNC "ThreadManForUser", 0x56C039B5, sceKernelCreateVpl
    IMPORT_FUNC "ThreadManForUser", 0xBED27435, sceKernelAllocateVpl
    IMPORT_FUNC "ThreadManForUser", 0xAF36D708, sceKernelTryAllocateVpl
    IMPORT_FUNC "ThreadManForUser", 0xB736E9FF, sceKernelFreeVpl
    IMPORT_FUNC "ThreadManForUser", 0x39810265, sceKernelReferVplStatus
    IMPORT_FUNC "ThreadManForUser", 0x89B3D48C, sceKernelDeleteVpl
    IMPORT_FUNC "ThreadManForUser", 0x20FFF560, sceKernelCreateVTimer
    IMPORT_FUNC "ThreadManForUser", 0xC68D9437, sceKernelStartVTimer
    IMPORT_FUNC "ThreadManForUser", 0xD0AEEE87, sceKernelStopVTimer
    IMPORT_FUNC "ThreadManForUser", 0x5F32BEAA, sceKernelReferVTimerStatus
    IMPORT_FUNC "ThreadManForUser", 0x328F9E52, sceKernelDeleteVTimer
    IMPORT_FUNC "ThreadManForUser", 0x383F7BCC, sceKernelTerminateDeleteThread
    IMPORT_FUNC "ThreadManForUser", 0x8FFDF9A2, sceKernelCancelSema
    IMPORT_FUNC "ThreadManForUser", 0x402FCF22, sceKernelWaitEventFlag
    IMPORT_FUNC "ThreadManForUser", 0xCD203292, sceKernelCancelEventFlag
    IMPORT_FUNC "ThreadManForUser", 0x94AA61EE, sceKernelGetThreadCurrentPriority
