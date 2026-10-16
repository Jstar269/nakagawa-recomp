/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceLibFont import block. The Makefile links it only into the font-list
   case (font_list.h), which asks the console's own font library for the system
   font list. The PSPSDK ships no sceLibFont stubs or header, so the five calls
   the case makes are declared here and prototyped in font_list.h.

   The rules from threadman_user_imports.S apply: every sceLibFont function a
   case linking this file calls must be declared here, and the build-time gate
   (tools/psp_oracle/user_mode_imports.py, --exact-library) pins the block to
   exactly these five NIDs, with --forbid-library sceReg refusing any registry
   import in the same module.

   NID sources. Each NID is the first four bytes, little-endian, of SHA-1 of the
   export name: the derivation rule of tools/nid_corpus.json, where all five
   names are hash-verified entries. All five match src/rt/nid_names.h. The
   0x40010000 version/attribute word is the one the PSPSDK uses for its
   user-library imports (libpspdisplay.a and display_user_imports.S). */

#include "pspimport.s"

    IMPORT_START "sceLibFont", 0x40010000
    IMPORT_FUNC  "sceLibFont", 0x67F17ED7, sceFontNewLib
    IMPORT_FUNC  "sceLibFont", 0x574B6FBC, sceFontDoneLib
    IMPORT_FUNC  "sceLibFont", 0x27F6E642, sceFontGetNumFontList
    IMPORT_FUNC  "sceLibFont", 0xBC75D85B, sceFontGetFontList
    IMPORT_FUNC  "sceLibFont", 0x5333322D, sceFontGetFontInfoByIndexNumber
