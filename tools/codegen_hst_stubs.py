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

``HST_GAME_LOOP_ADDR`` is the one stub that wraps a translated body instead of replacing it:
``codegen.py`` emits the real function renamed ``f_000468c8_real`` and appends
``HST_GAME_LOOP_WRAPPER``, which calls it in a frame loop.

Retired stubs, kept here so nobody reinstates them: 0x0001034c (heap-statistics walk skip) and
0x001d9eb0 (title backdrop chooser postcondition) were retired under #363, because the walk
runs over the retail allocator's statically linked, untouched bins once the allocator is
host-bridged, and the chooser never yields 0; both now translate as written.  0x0001a5f8 and
0x0001c008 had constant-return stubs until 2026-07-18.  A Ghidra-assisted review showed both
shadow real, fully translatable code: 0x1c008 is ``jr ra; sw a1,0x4028(a0)`` (the stub dropped
the delay-slot store) and 0x1a5f8 is a computed-goto resume point reached through .data pointer
tables.  Both are discovered entries and translate faithfully;
``tools/test_codegen_no_shadow_stubs.py`` keeps them so.
"""

HST_GAME_LOOP_ADDR = 0x000468C8

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


HST_GAME_LOOP_WRAPPER = """void f_000468c8(CpuState *s) {  /* custom stub: main_RunGameLoop - infinite frame loop */
    for (;;) {
        SR_YIELD(s, 0x000468c8u);
        f_000468c8_real(s);
        SR_TAIL_DRAIN(s);
    }
}"""

HST_CUSTOM_STUBS = {
    0x000011B0: """void f_000011b0(CpuState *s) {  /* custom stub: __register_frame_info bypass */
    s->pc = s->r[31];
}""",

    0x0000260C: """void f_0000260c(CpuState *s) {  /* custom stub: exception helper bypass */
    s->pc = s->r[31];
}""",

    0x0000FE3C: """void f_0000fe3c(CpuState *s) {  /* custom stub: _getmodreent / FileIO_GetState */
    if (s->r[31] == 0x000104c4u || s->r[31] == 0x000104f4u) {
        /* Allocator calls always use the main thread's global impure reent */
        s->r[2] = 0x002cf338u;
    } else {
        extern uint32_t sr_thread_k0(void);
        uint32_t k0 = s->r[26];
        if (k0 < 0x08000000u || k0 >= 0x10000000u) k0 = sr_thread_k0();
        uint32_t ptr = k0 ? MEM_R32(k0 + 4) : 0;
        if (!ptr) ptr = MEM_R32(0x002cf6b8u);
        if (!ptr) ptr = 0x002cf338u;
        s->r[2] = ptr;
    }
}""",

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

    0x00015EA0: """void f_00015ea0(CpuState *s) {
    /* Host-side tokenizer matching FUN_00015ea0 (EBOOT.BIN.dec.c:22965).
     * str is the CSV buffer pointer; delim is the delimiter string. First
     * call (str != 0) builds a cached token table by walking the buffer
     * once. Subsequent calls return one token pointer per yield. */
    uint32_t str = s->r[4];
    uint32_t delim = s->r[5];
    static uint32_t s_tokens[16384];
    static uint32_t s_ntokens = 0;
    static uint32_t s_tok_idx = 0;
    if (str != 0) {
        s_ntokens = 0;
        s_tok_idx = 0;
        uint32_t p = str;
        uint32_t d = delim;
        while (MEM_R8(p) != 0 && s_ntokens < 16384) {
            while (MEM_R8(p) != 0) {
                int is_delim = 0;
                for (uint32_t dd = d; MEM_R8(dd) != 0; dd++) {
                    if (MEM_R8(p) == MEM_R8(dd)) { is_delim = 1; break; }
                }
                if (!is_delim) break;
                p++;
            }
            if (MEM_R8(p) == 0) break;
            s_tokens[s_ntokens++] = p;
            while (MEM_R8(p) != 0) {
                int is_delim = 0;
                for (uint32_t dd = d; MEM_R8(dd) != 0; dd++) {
                    if (MEM_R8(p) == MEM_R8(dd)) { is_delim = 1; break; }
                }
                if (is_delim) { MEM_W8(p, 0); p++; break; }
                p++;
            }
        }
    }
    s->r[2] = (s_tok_idx < s_ntokens) ? s_tokens[s_tok_idx++] : 0u;
    s->pc = s->r[31];
}""",

    0x000143B0: """void f_000143b0(CpuState *s) {  /* custom stub: guest sprintf */
    sr_guest_sprintf(s);
}""",

    0x000149A8: """void f_000149a8(CpuState *s) {  /* custom stub: strcpy native */
    /* Boot's file-open wrapper copies its source path here before adding the
     * host0: prefix.  Treating this routine as strcmp left the destination
     * uninitialized and corrupted every resource path during startup. */
    uint32_t dst0 = s->r[4], dst = dst0, src = s->r[5];
    if (dst != 0u && src != 0u) {
        for (uint32_t n = 0; n < 0x00100000u; n++) {
            uint8_t ch = MEM_R8(src++);
            MEM_W8(dst++, ch);
            if (ch == 0u) break;
        }
    }
    s->r[2] = dst0;
    s->pc = s->r[31];
}""",

    **_per_address(_MEMCPY_NATIVE, (0x00011090, 0x000110DC)),
    **_per_address(_MEMSET_NATIVE, (0x000114C0, 0x000114A8)),
}
