/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceCtrl import block for CASE=hle-ctrl-latch. Only the two latch
   queries are declared: no sampling-mode or buffer Set function is imported.
   Library name and version word match the PSPSDK libpspctrl.a stubs; the
   kernel archive libpspctrl_driver.a is never linked. */

#include "pspimport.s"

    IMPORT_START "sceCtrl", 0x40010000
    IMPORT_FUNC  "sceCtrl", 0x0B588501, sceCtrlReadLatch
    IMPORT_FUNC  "sceCtrl", 0xB1D0E5CD, sceCtrlPeekLatch
