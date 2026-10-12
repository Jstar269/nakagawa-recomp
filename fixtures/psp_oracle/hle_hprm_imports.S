/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceHprm import block for CASE=hle-hprm (remote, headphone and
   microphone presence queries). Library name and version word match the
   PSPSDK libpsphprm.a stubs. The kernel archive libpsphprm_driver.a is never
   linked. */

#include "pspimport.s"

    IMPORT_START "sceHprm", 0x40010000
    IMPORT_FUNC  "sceHprm", 0x208DB1BD, sceHprmIsRemoteExist
    IMPORT_FUNC  "sceHprm", 0x7E69EDA4, sceHprmIsHeadphoneExist
    IMPORT_FUNC  "sceHprm", 0x219C58F1, sceHprmIsMicrophoneExist
