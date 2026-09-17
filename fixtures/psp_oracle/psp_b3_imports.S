/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* PSP-B3 extends the PSP-B2 ThreadManForUser block with the message-pipe,
   mailbox, sleep/wakeup, alarm and thread-control calls it adds. NIDs were read
   from the PSPSDK libpspuser.a .rodata.sceNid members and match
   src/rt/nid_names.h. */

#include "psp_b2_imports.S"

    IMPORT_FUNC "ThreadManForUser", 0x7C0DC2A0, sceKernelCreateMsgPipe
    IMPORT_FUNC "ThreadManForUser", 0x8125221D, sceKernelCreateMbx
    IMPORT_FUNC "ThreadManForUser", 0x86255ADA, sceKernelDeleteMbx
    IMPORT_FUNC "ThreadManForUser", 0xE9B3061E, sceKernelSendMbx
    IMPORT_FUNC "ThreadManForUser", 0x18260574, sceKernelReceiveMbx
    IMPORT_FUNC "ThreadManForUser", 0x0D81716A, sceKernelPollMbx
    IMPORT_FUNC "ThreadManForUser", 0x87D4DD36, sceKernelCancelReceiveMbx
    IMPORT_FUNC "ThreadManForUser", 0xA8E8C846, sceKernelReferMbxStatus
    IMPORT_FUNC "ThreadManForUser", 0x9ACE131E, sceKernelSleepThread
    IMPORT_FUNC "ThreadManForUser", 0xD59EAD2F, sceKernelWakeupThread
    IMPORT_FUNC "ThreadManForUser", 0xFCCFAD26, sceKernelCancelWakeupThread
    IMPORT_FUNC "ThreadManForUser", 0x2C34E053, sceKernelReleaseWaitThread
    IMPORT_FUNC "ThreadManForUser", 0x369ED59D, sceKernelGetSystemTimeLow
    IMPORT_FUNC "ThreadManForUser", 0x6652B8CA, sceKernelSetAlarm
    IMPORT_FUNC "ThreadManForUser", 0x7E65B999, sceKernelCancelAlarm
    IMPORT_FUNC "ThreadManForUser", 0x71BC9871, sceKernelChangeThreadPriority
    IMPORT_FUNC "ThreadManForUser", 0x9944F31F, sceKernelSuspendThread
    IMPORT_FUNC "ThreadManForUser", 0x75156E8F, sceKernelResumeThread
    IMPORT_FUNC "ThreadManForUser", 0x3B183E26, sceKernelGetThreadExitStatus
    IMPORT_FUNC "ThreadManForUser", 0x616403BA, sceKernelTerminateThread
