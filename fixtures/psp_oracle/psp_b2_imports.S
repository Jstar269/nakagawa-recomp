/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* PSP-B2 reuses the complete PSP-B1 ThreadManForUser block (which itself
   extends the Phase-B block) and appends the wait/cancel calls it adds, so the
   library stays one contiguous stub run. NIDs were read from the PSPSDK
   libpspuser.a .rodata.sceNid members and match src/rt/nid_names.h. */

#include "psp_b1_imports.S"

    IMPORT_FUNC "ThreadManForUser", 0x8FFDF9A2, sceKernelCancelSema
    IMPORT_FUNC "ThreadManForUser", 0x402FCF22, sceKernelWaitEventFlag
    IMPORT_FUNC "ThreadManForUser", 0xCD203292, sceKernelCancelEventFlag
    IMPORT_FUNC "ThreadManForUser", 0x94AA61EE, sceKernelGetThreadCurrentPriority
