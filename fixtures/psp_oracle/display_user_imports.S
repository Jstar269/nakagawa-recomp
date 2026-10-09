/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceDisplay import block. The Makefile links it only into the cases
   that call a sceDisplay function the PSPSDK does not ship (today only
   kernel-misc, for sceDisplayWaitVblankStartMultiCB).

   The probes are user-mode modules, so they may import only user (syscall)
   libraries. The kernel library sceDisplay_driver is not visible to them: a
   user-mode PRX that imports it fails to load on the PSP with 0x8002013C
   (library not found). The functions below are exported to user mode by the
   sceDisplay library under the same NIDs. tools/psp_oracle/user_mode_imports.py
   rejects any kernel-only import in a user-mode probe at link time.

   The rules from threadman_user_imports.S apply here. IMPORT_START emits a
   global __stub_module_sceDisplay, which stops the linker from pulling the
   SDK's header member for sceDisplay, so any SDK sceDisplay function stub that
   is still pulled would land in a second, non-adjacent stub run that
   psp-fixup-imports cannot describe. Every sceDisplay function that a case
   linking this file calls must therefore be declared here.

   NID sources. Each NID is the first four bytes, little-endian, of SHA-1 of
   the export name: the derivation rule of tools/nid_corpus.json, where both
   names are hash-verified entries. Both match src/rt/nid_names.h. 0x7ED59BC4
   sceDisplaySetHoldMode is also the user sceDisplay stub in the PSPSDK
   libpspdisplay.a. The 0x40010000 version/attribute word is the one
   libpspdisplay.a uses for its sceDisplay import. */

#include "pspimport.s"

    IMPORT_START "sceDisplay", 0x40010000
    IMPORT_FUNC  "sceDisplay", 0x7ED59BC4, sceDisplaySetHoldMode
    IMPORT_FUNC  "sceDisplay", 0x77ED8B3A, sceDisplayWaitVblankStartMultiCB
