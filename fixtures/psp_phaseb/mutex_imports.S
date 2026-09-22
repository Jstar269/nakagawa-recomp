/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Phase-B's complete ThreadManForUser import block.

   Why this file owns *every* ThreadManForUser import and not just the mutex
   NIDs.

   PSPSDK ships mutex stubs only in libpspthreadman_kernel_660.a, and those
   import from ThreadManForKernel.  A user-mode probe that used them would
   break the campaign's THREADMAN_KERNEL_IMPORTS = 0 invariant, so Phase-B has
   always declared its own ThreadManForUser mutex stubs here.

   Declaring IMPORT_START for a library the SDK also provides has a
   consequence that was not accounted for.  The macro emits a *global*
   __stub_module_ThreadManForUser, so once this object is on the link line the
   linker never pulls libpspuser.a(ThreadManForUser_0000.o) -- the SDK's header
   member for that library.  The SDK's individual ThreadManForUser function
   stubs are still pulled, they bind to the header defined here, and they land
   at libpspuser's position in .sceStub.text, far from the eight emitted here.

   The result was two non-adjacent runs of ThreadManForUser stubs, 8 then 25.
   psp-fixup-imports walks .sceStub.text in address order and tracks the last
   module it saw (pLastImport); a module that reappears after another module is
   "stubs out of order".  It wrote func_count = 33 on the single header but
   could only point nid_table/stub_table at the first run, so the declared
   count ran off the end of its own region and into the next library's.  The
   linked table said ThreadManForUser had 33 functions in an 8-entry region and
   SysMemUserForUser had 4 in a 29-entry region.

   Contiguity for a library can only be guaranteed when all of its stubs come
   from one input file, so this file now declares the complete set Phase-B
   links: the eight mutex NIDs, plus every ThreadManForUser function the probe
   and the PSPSDK CRT reference.  The SDK's ThreadManForUser members are then
   never pulled, and the library has exactly one contiguous run.

   The NIDs below were extracted mechanically from the SDK archive members that
   the previous link actually pulled

     psp-ar x libpspuser.a && psp-objdump -s -j .rodata.sceNid ThreadManForUser_NNNN.o

   and every one was independently corroborated against this repository's own
   NID tables (33/33 agree, 0 disagree).  They are not transcribed by hand.

   If Phase-B or the CRT ever references a ThreadManForUser function that is
   absent here, the linker pulls that SDK member again, the two-run split
   returns, and both psp-fixup-imports and the structural verifier
   (tools/boot12_verify_imports.py) fail.  The failure is loud by construction;
   add the missing NID here rather than silencing it. */

#include "pspimport.s"

    IMPORT_START "ThreadManForUser", 0x40010000

    /* Custom mutex ABI: user-mode NIDs the SDK only ships for the kernel. */
    IMPORT_FUNC  "ThreadManForUser", 0xB7D098C6, sceKernelCreateMutex
    IMPORT_FUNC  "ThreadManForUser", 0xF8170FBE, sceKernelDeleteMutex
    IMPORT_FUNC  "ThreadManForUser", 0xB011B11F, sceKernelLockMutex
    IMPORT_FUNC  "ThreadManForUser", 0x5BF4DD27, sceKernelLockMutexCB
    IMPORT_FUNC  "ThreadManForUser", 0x0DDCD2C9, sceKernelTryLockMutex
    IMPORT_FUNC  "ThreadManForUser", 0x6B30100F, sceKernelUnlockMutex
    IMPORT_FUNC  "ThreadManForUser", 0x87D9223C, sceKernelCancelMutex
    IMPORT_FUNC  "ThreadManForUser", 0xA9C2CB9A, sceKernelReferMutexStatus

    /* Every other ThreadManForUser function Phase-B and the CRT link. */
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
