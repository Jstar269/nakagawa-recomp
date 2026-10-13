/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceGe_user import block for CASE=hle-ge-edram. The eDRAM size and
   address queries and sceGeEdramSetAddrTranslation are the only GE functions
   declared: the translation width is the one GE state the probe changes, and
   it restores the original width before exit. Library name and version word
   match the PSPSDK libpspge.a stubs; libpspge_driver.a is never linked. */

#include "pspimport.s"

    IMPORT_START "sceGe_user", 0x40010000
    IMPORT_FUNC  "sceGe_user", 0x1F6752AD, sceGeEdramGetSize
    IMPORT_FUNC  "sceGe_user", 0xE47E40E4, sceGeEdramGetAddr
    IMPORT_FUNC  "sceGe_user", 0xB77905EA, sceGeEdramSetAddrTranslation
