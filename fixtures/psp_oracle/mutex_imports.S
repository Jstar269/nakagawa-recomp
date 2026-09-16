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
