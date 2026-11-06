# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-11-01.
# See NOTICE.md for upstream lineage and modification provenance.

"""The hand-written C bodies ``tools/codegen.py`` emits in place of translated code under
``--profile=hst``.

Each entry is the complete text of one generated function: ``HST_CUSTOM_STUBS`` maps a guest
address to it.  The text is data, not logic: ``codegen.py`` looks the address up and appends it
to the chunk unchanged, so editing a body here changes the generated C exactly as editing it in
``codegen.py`` did.  ``tools/compat_overrides.py`` (``CODEGEN_CUSTOM_STUBS``) records why each
address is coupled to the title, and ``tools/test_compat_manifest.py`` checks the two agree.

Retired stubs, kept here so nobody reinstates them: 0x000468c8 (main_RunGameLoop scheduler wrapper, which ran the translated body renamed ``f_000468c8_real`` inside a frame loop with a yield per iteration) was retired under #363: the worker thread's own loop yields through its syscalls, and the function translates as written.  0x000011b0 (__register_frame_info bypass), 0x0000260c (exception helper bypass), 0x0000fe3c (_getmodreent), 0x00015ea0 (CSV tokenizer) and 0x000149a8 (strcpy) were retired under #363: the guest's own bodies run as written.  0x0001034c (heap-statistics walk skip) and
0x001d9eb0 (title backdrop chooser postcondition) were retired under #363, because the walk
runs over the retail allocator's statically linked, untouched bins once the allocator is
host-bridged, and the chooser never yields 0; both now translate as written.  0x0001a5f8 and
0x0001c008 had constant-return stubs until 2026-07-18.  A Ghidra-assisted review showed both
shadow real, fully translatable code: 0x1c008 is ``jr ra; sw a1,0x4028(a0)`` (the stub dropped
the delay-slot store) and 0x1a5f8 is a computed-goto resume point reached through .data pointer
tables.  Both are discovered entries and translate faithfully;
``tools/test_codegen_no_shadow_stubs.py`` keeps them so.
"""

# One body shared by 0x00011090, 0x000110DC; "@ADDR@" is the address in
# lower-case hex, as in the function name.
_MEMCPY_NATIVE = """void f_@ADDR@(CpuState *s) {  /* custom stub: memcpy native */
    uint32_t dest = s->r[4];
    sr_native_memmove(dest, s->r[5], s->r[6], 0x@ADDR@u);
    s->r[2] = dest;
    s->pc = s->r[31];
}"""

# One body shared by 0x000114C0, 0x000114A8; "@ADDR@" is the address in
# lower-case hex, as in the function name.
_MEMSET_NATIVE = """void f_@ADDR@(CpuState *s) {  /* custom stub: sceKernelMemset native */
    uint32_t dest = s->r[4];
    sr_native_memset(dest, (uint8_t)(s->r[5] & 0xFF), s->r[6], 0x@ADDR@u);
    s->r[2] = dest;
    s->pc = s->r[31];
}"""


def _per_address(template, addresses):
    return {a: template.replace("@ADDR@", f"{a:08x}") for a in addresses}


HST_CUSTOM_STUBS = {



    0x00010738: """void f_00010738(CpuState *s) {  /* custom stub: _malloc_r(reent, size) -> sr_newlib_malloc bridge
     * This runtime owns the arena metadata. Retail _memalign_r/_realloc_r edit dlmalloc
     * headers directly, where bit 0 means PREV_INUSE; the host header uses bit 0 for the
     * current block's allocation state. Keep the whole metadata-manipulating API on one ABI.
     * malloc()'s retail wrapper saves its own caller RA at sp+4 before calling _malloc_r.
     * operator new then adds a 0x20-byte frame and saves its caller at +0xc, which is
     * sp+0x1c from here. Unwrap only those exact verified return PCs; direct _malloc_r
     * callers retain their immediate RA, so tracing gains provenance without changing
     * guest state. */
    uint32_t owner_ra = s->r[31] == 0x000104d0u
        ? MEM_R32(s->r[29] + 0x00000004u) : s->r[31];
    if (owner_ra == 0x00000bf4u || owner_ra == 0x00000c5cu)
        owner_ra = MEM_R32(s->r[29] + 0x0000001cu);
    s->r[2] = sr_newlib_malloc(s->r[5], owner_ra);
    s->pc = s->r[31];
}""",

    0x0000F538: """void f_0000f538(CpuState *s) {  /* custom stub: _free_r(reent, ptr) -> sr_newlib_free bridge
     * Reinstated 2026-07-16 alongside f_00010738 -- see that stub's comment. */
    uint32_t owner_ra = s->r[31] == 0x00010500u
        ? MEM_R32(s->r[29] + 0x00000004u) : s->r[31];
    if (owner_ra == 0x00000a14u)
        owner_ra = MEM_R32(s->r[29] + 0x0000001cu);
    sr_newlib_free(s->r[5], owner_ra);
    s->pc = s->r[31];
}""",

    0x000101C4: """void f_000101c4(CpuState *s) {  /* custom stub: _memalign_r(reent, alignment, size)
     * The translated retail body carves dlmalloc chunks and writes PREV_INUSE into the
     * following header. That bit is incompatible with the host arena's current-block flag. */
    s->r[2] = sr_newlib_memalign(s->r[5], s->r[6], s->r[31]);
    s->pc = s->r[31];
}""",

    0x00013524: """void f_00013524(CpuState *s) {  /* custom stub: _realloc_r(reent, ptr, size)
     * Retail realloc also walks, unlinks, and rewrites dlmalloc headers, so it must use
     * the same host-owned metadata ABI as malloc/free/memalign. */
    s->r[2] = sr_newlib_realloc(s->r[5], s->r[6], s->r[31]);
    s->pc = s->r[31];
}""",


    0x000143B0: """void f_000143b0(CpuState *s) {  /* custom stub: guest sprintf */
    sr_guest_sprintf(s);
}""",


    **_per_address(_MEMCPY_NATIVE, (0x00011090, 0x000110DC)),
    **_per_address(_MEMSET_NATIVE, (0x000114C0, 0x000114A8)),
}
