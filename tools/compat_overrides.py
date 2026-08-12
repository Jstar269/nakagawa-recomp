# SPDX-License-Identifier: GPL-2.0-or-later

"""Semantic-debt manifest: every game-address-specific override in the runtime.

2026-07-17. This is the "make false progress impossible" inventory: every custom
codegen stub, guest-instruction patch, and dispatch hook that makes the recompiled
game diverge from a literal translation of the retail binary. Each entry answers
"why does this exist, and is it faithful, a boundary, a bug-shaped patch, a log
line, or unexplained?"

Format is a plain Python data module (not TOML/JSON) on purpose: the manifest is
read directly by the audit and should not depend on a serialization round-trip.
tools/test_compat_manifest.py imports this module directly and cross-checks
OVERRIDES against the two authoritative,
mechanically-extractable sources of hooks in the tree:
  - tools/codegen.py: GUEST_PATCHES, HST entry-role metadata,
    host_stubs.HST_SIMPLE_STUBS, and the
    per-address custom stubs between the "--- CUSTOM STUBS START/END ---"
    markers in emit_function's driver loop.
  - src/rt/recomp.c: the g_exact_hooks[]/g_range_hooks[] DispatchHook tables.
CI fails if either source contains an address this manifest does not list, or
if this manifest lists an address neither source actually contains (stale entry).

Categories (pick exactly one per entry):
  faithful_abi_bridge         -- reproduces the retail binary's own semantics;
                                  exists because codegen/ABI plumbing can't
                                  express it directly (calling convention,
                                  native reimplementation of a translated-wrong
                                  library routine, scheduler/host integration).
                                  Does NOT change what the game decides.
  hle_boundary                -- a necessary substitution at a genuine
                                  host/guest boundary (syscall-like library
                                  entry, host filesystem bridge, allocator
                                  bridge). The guest has no faithful path here
                                  by construction (no real kernel underneath).
  temporary_compatibility_patch -- papers over a specific, still-open bug
                                  (missing HLE state, a codegen gap, unfound
                                  root cause) by forcing a branch, faking a
                                  return, or skipping guest work. Tracked
                                  against an issue; removing the patch and
                                  seeing whether the game still works is the
                                  intended way to retire it.
  diagnostic                  -- read-only, env-gated, no control-flow or
                                  memory side effects. Safe by construction;
                                  listed for completeness, not because it is
                                  risky.
  unexplained                 -- no rationale is recorded in the source. Flag
                                  for follow-up, not a judgement that it's
                                  wrong.

Every entry's `test` field names the regression test that pins it, or "none" if
it does not have one yet (a real gap -- see ISSUES.md, not silently swept in).
"""

CATEGORIES = {
    "faithful_abi_bridge",
    "hle_boundary",
    "temporary_compatibility_patch",
    "diagnostic",
    "unexplained",
}

# --- tools/codegen.py: GUEST_PATCHES (instruction-level overrides) ----------
GUEST_PATCHES = [
    dict(address=0x00010950, layer="codegen_instruction", category="temporary_compatibility_patch",
         source="tools/codegen.py:GUEST_PATCHES", name="bypass worker frame-init spin loop",
         reason="force the branch condition to skip a spin loop at 0x10950 (worker frame init)",
         test="none", owner_issue="ISSUES.md #5.1"),
    dict(address=0x00048320, layer="codegen_instruction", category="temporary_compatibility_patch",
         source="tools/codegen.py:GUEST_PATCHES", name="force single-iteration pass",
         reason="inject a forced single-iteration exit at 0x48320",
         test="none", owner_issue="ISSUES.md #5.1"),
    dict(address=0x0004cdc8, layer="codegen_instruction", category="hle_boundary",
         source="tools/codegen.py:GUEST_PATCHES", name="route asset reads through host filesystem",
         reason="force the branch that selects the host0 (sceIoOpen-backed) asset path; "
                "the guest-only host filesystem implementation does not exist to select instead",
         test="none", owner_issue="ISSUES.md P0 table root 0x0034a84c"),
]

# --- tools/codegen.py: custom per-address stubs -----------------------------
CODEGEN_CUSTOM_STUBS = [
    dict(address=0x000011b0, category="hle_boundary", name="__register_frame_info bypass",
         reason="C++ exception-frame registration; no real dynamic unwinder underneath the "
                "recompiled runtime for it to register with",
         test="none"),
    dict(address=0x0000260c, category="hle_boundary", name="exception helper bypass",
         reason="same rationale as 0x11b0 -- libc exception-support internal, no-op here",
         test="none"),
    dict(address=0x0000fe3c, category="faithful_abi_bridge", name="_getmodreent / FileIO_GetState",
         reason="native reimplementation of newlib's per-thread reentrancy-struct lookup, "
                "adapted to this runtime's k0-based thread model",
         test="none"),
    dict(address=0x00010738, category="hle_boundary", name="_malloc_r -> sr_newlib_malloc bridge",
         reason="bridges retail dlmalloc to the host-owned arena. Retail memalign/realloc edit "
                "dlmalloc headers directly, where bit 0 is PREV_INUSE; the host header uses bit "
                "0 for the current block's allocation state. Mixing those metadata ABIs "
                "deterministically quarantined a valid free list.",
         test="tools/test_codegen_retail_allocator.py"),
    dict(address=0x0000f538, category="hle_boundary", name="_free_r -> sr_newlib_free bridge",
         reason="see 0x10738 -- same bridge, the free half",
         test="tools/test_codegen_retail_allocator.py"),
    dict(address=0x000101c4, category="hle_boundary",
         name="_memalign_r -> sr_newlib_memalign bridge",
         reason="the retail body directly carves dlmalloc chunks. A captured write at pc "
                "0x000102b0 set PREV_INUSE on a host free header, whose bit 0 instead means "
                "current block allocated; the host bridge preserves alignment without crossing "
                "allocator metadata ABIs.",
         test="tools/test_codegen_retail_allocator.py"),
    dict(address=0x00013524, category="hle_boundary",
         name="_realloc_r -> sr_newlib_realloc bridge",
         reason="the retail body walks, unlinks, and rewrites dlmalloc chunks, so it must share "
                "the host metadata ABI used by malloc/free/memalign.",
         test="tools/test_codegen_retail_allocator.py"),
    dict(address=0x00015ea0, category="faithful_abi_bridge", name="CSV tokenizer (strtok-family)",
         reason="native reimplementation of FUN_00015ea0's cached-token-table tokenizer",
         test="none"),
    dict(address=0x000143b0, category="faithful_abi_bridge", name="guest sprintf",
         reason="the translated original formatter corrupted %s arguments during boot; this "
                "native implementation preserves PSP EABI varargs placement (r6..r11, then stack), "
                "including aligned two-word double arguments for floating conversions",
         test="tools/test_guest_printf.py"),
    dict(address=0x00046d14, category="diagnostic", name="game loop entry trace",
         reason="unconditional (non-env-gated) one-line fprintf marking L_00046d14 entry; "
                "debug litter, harmless but should be gated or removed",
         test="none"),
    dict(address=0x0001034c, category="temporary_compatibility_patch", name="skip heap-statistics walk",
         reason="the guest free-list can be incomplete during bring-up; walking it for "
                "mallinfo-style counters must not block game initialization",
         test="none"),
    # 0x0001a5f8 / 0x0001c008: constant-return stubs removed 2026-07-18 after a
    # Ghidra-assisted review proved both shadowed real translatable code (a
    # delay-slot setter and a computed-goto resume point). The real translations
    # are guarded by tools/test_codegen_no_shadow_stubs.py; "unexplained" count
    # is now zero.
    dict(address=0x000468c8, category="faithful_abi_bridge", name="main_RunGameLoop scheduler wrapper",
         reason="wraps the real per-frame body in a SR_YIELD loop so the cooperative scheduler "
                "gets a fiber-switch point every frame; structural, does not alter game logic",
         test="none"),
    dict(address=0x001d9eb0, category="temporary_compatibility_patch", name="title backdrop selector postcondition",
         reason="recovers from an impossible id=0 result from the retail chooser (every shipped "
                "title archive is numbered from 01) instead of leaving a stray text surface in "
                "the title background",
         test="none"),
    dict(address=0x00011090, category="faithful_abi_bridge", name="memcpy native",
         reason="native memmove-backed reimplementation, same semantics as the translated body",
         test="none"),
    dict(address=0x000110dc, category="faithful_abi_bridge", name="memcpy native (alias)",
         reason="see 0x11090",
         test="none"),
    dict(address=0x000114c0, category="faithful_abi_bridge", name="sceKernelMemset native",
         reason="native memset-backed reimplementation, same semantics as the translated body",
         test="none"),
    dict(address=0x000114a8, category="faithful_abi_bridge", name="sceKernelMemset native (alias)",
         reason="see 0x114c0",
         test="none"),
    dict(address=0x000149a8, category="faithful_abi_bridge", name="strcpy native",
         reason="the boot file-open wrapper's source path copy was previously miscompiled as "
                "strcmp, corrupting every resource path; this native strcpy replaces it",
         test="none"),
]

# --- tools/codegen.py: HST profile entry roles -----------------------------
# These are not behavior overrides: the listed addresses are translated from
# guest instructions.  They are nevertheless title-address-specific metadata
# and stay inventoried here so #20/#51 can audit and eventually retire the
# remaining manual analyzer seeds without conflating host entries with source
# functions.
HST_ENTRY_ROLES = [
    dict(address=0x0005A648, role="callable", owner=None,
         provenance="address-taken-tiny-leaf", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x00042998, role="callable", owner=None,
         provenance="address-taken-tiny-leaf", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x0003DB3C, role="callable", owner=None,
         provenance="address-taken-tiny-leaf", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x000E1724, role="callable", owner=None,
         provenance="address-taken-tiny-leaf", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x000E3B24, role="callable", owner=None,
         provenance="address-taken-tiny-leaf", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x00014430, role="callable", owner=None,
         provenance="address-taken-tiny-leaf", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x000310B0, role="resume", owner=0x00030FDC,
         provenance="private-elf-verified", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x00021C78, role="resume", owner=0x00021AC0,
         provenance="private-elf-verified", test="tools/test_codegen_entry_semantics.py"),
    dict(address=0x000B26A0, role="resume", owner=0x000B237C,
         provenance="private-elf-verified", test="tools/test_codegen_entry_semantics.py"),
]

# --- tools/host_stubs.py: HST_SIMPLE_STUBS ----------------------------------
HST_SIMPLE_STUBS = [
    dict(address=0x00015f98, category="temporary_compatibility_patch", name="Config_LoadGameSettings",
         reason="returns a static success value instead of performing real config load",
         test="none"),
    dict(address=0x0001c010, category="temporary_compatibility_patch", name="VFS_RegisterHeap",
         reason="no-op success; no VFS heap is actually registered",
         test="none"),
    dict(address=0x0001c0fc, category="temporary_compatibility_patch", name="VFS_RegisterBuffer",
         reason="no-op success; no VFS buffer is actually registered",
         test="none"),
    dict(address=0x0001c104, category="temporary_compatibility_patch", name="Config_LoadProfile",
         reason="returns a static success value instead of performing real profile load",
         test="none"),
    dict(address=0x0001c810, category="temporary_compatibility_patch", name="TexCache_Initialize",
         reason="no-op success; no texture cache is actually initialized here",
         test="none"),
    dict(address=0x0001c818, category="temporary_compatibility_patch", name="VFS_RegisterCallback",
         reason="no-op success; no VFS callback is actually registered",
         test="none"),
    dict(address=0x0001c560, category="temporary_compatibility_patch", name="main_GraphicsInit",
         reason="returns a static success value instead of performing real graphics init "
                "(the real GPU backend is brought up elsewhere in the host runtime)",
         test="none"),
    dict(address=0x0001c604, category="temporary_compatibility_patch", name="World_LoadInitialState",
         reason="no-op success; no world initial state is actually loaded",
         test="none"),
]

# --- src/rt/recomp.c: g_exact_hooks[] / g_range_hooks[] DispatchHook tables -
DISPATCH_HOOKS = [
    dict(address=0x00304290, category="temporary_compatibility_patch", name="INIT_LANG",
         reason="runs the real language-init function, then force-writes the JP-locale flag "
                "at 0x30fbfd that the CSV loader needs for asset-path selection",
         test="none"),
    dict(address=0x000104b0, category="diagnostic", name="ALLOC_REQ",
         reason="SR_ALLOC_TRACE-gated log of malloc requests; always falls through unchanged",
         test="none"),
    dict(address=0x000104e0, category="diagnostic", name="FREE_REQ",
         reason="SR_ALLOC_TRACE-gated log of free requests; always falls through unchanged",
         test="none"),
    dict(address=0x00030950, category="faithful_abi_bridge", name="TC30950",
         reason="a tail-call target lands at a callee's +8 entry point (past its prologue) "
                "that codegen does not separately register; redirects to the real registered "
                "entry with an equivalent net stack delta",
         test="none"),
    dict(address=0x0001b6c4, category="temporary_compatibility_patch", name="HINSERT",
         reason="guards a linear-probe hash insert against an infinite wrap when the table's "
                "capacity is 0, returning early instead of letting the real routine spin",
         test="none"),
    dict(address=0x0001b584, category="diagnostic", name="HFILL",
         reason="traces hash_fill entry, then always delegates to the real function",
         test="none"),
    dict(address=0x656a6f72, category="temporary_compatibility_patch", name="NULL_CALL_A",
         reason="dispatch to a garbage/ASCII-looking target (corrupted vtable slot or "
                "misread data) is logged and turned into a harmless return instead of a crash",
         test="none"),
    dict(address=0x00000000, category="temporary_compatibility_patch", name="NULL_CALL_B",
         reason="dispatch to a literal null function pointer is logged and turned into a "
                "harmless return instead of a crash",
         test="none"),
    dict(address=0x32305f34, category="temporary_compatibility_patch", name="SCEDMAC",
         reason="a known bad target (an sceDmac string literal misread as a function pointer) "
                "is treated as a no-op",
         test="none"),
    dict(address=0x00018130, category="diagnostic", name="FMT_TRACE",
         reason="traces the format-parser integer handler, then always delegates to the real function",
         test="none"),
    dict(address=0x0000ef40, category="temporary_compatibility_patch", name="MODTABLE_WALK",
         reason="replaces the module-registration table walker with a one-shot success return; "
                "defensive fallback for a walk that would otherwise loop indefinitely against "
                "an unseeded reent table",
         test="none"),
    dict(address=0x002cf338, category="temporary_compatibility_patch", name="_REENT_DATA",
         reason="the newlib _reent struct's address (a DATA address, never a real code target) "
                "is dispatched as a function pointer by the module-table walker; caught and "
                "returned success instead of accumulating hundreds of miss messages",
         test="none"),
    dict(address=0x0B000100, category="temporary_compatibility_patch", name="MOD_STUB",
         reason="returns success for an unregistered PRX module function pointer",
         test="none"),
    dict(address=0x00000ec0, category="diagnostic", name="THUNK_A",
         reason="logs thunk dispatches through the function-pointer slot at 0x2CED08; always "
                "falls through to normal dispatch",
         test="none"),
    dict(address=0x00000ee4, category="diagnostic", name="THUNK_B",
         reason="see 0xec0 (THUNK_A) -- same trace hook, second thunk slot",
         test="none"),
    dict(address=0x0000100c, category="hle_boundary", name="PLT_TRAMP",
         reason="an unresolved PLT trampoline slot (no HLE handler registered for that "
                "import); returns an honest r2=0 failure to the caller rather than looping "
                "self-referentially forever",
         test="none"),
    dict(address=0x00102e1c, category="diagnostic", name="PLT_WALK_1",
         reason="registered with a no-op handler (always falls through); its only effect is "
                "being a named, acknowledged entry in the miss log rather than an anonymous one",
         test="none"),
    dict(address=0x001030b0, category="diagnostic", name="PLT_WALK_2",
         reason="see 0x102e1c (PLT_WALK_1)",
         test="none"),
]
DISPATCH_RANGE_HOOKS = [
    dict(address=None, category="temporary_compatibility_patch", name="RESOURCE_HANDLE",
         reason="a broad pattern match (top byte 0x33/0x44/0x55/0x88, or two specific magic "
                "values) that treats any dispatch target shaped like a packed ECS/resource "
                "handle as a harmless no-op instead of a crash. The single riskiest entry in "
                "this manifest: it is a pattern, not a specific address, so it can silently "
                "swallow a genuine bug anywhere those bit patterns occur as a target.",
         test="none"),
]

# --- src/rt/sched.c and src/rt/recomp.c: behavior-altering scheduler hooks --
# Documented manually (not mechanically cross-checked by test_compat_manifest.py --
# see that file's header for why the automated gate is scoped to the two sources
# above only).
SCHEDULER_HOOKS = [
    dict(address=0x000468c8, category="temporary_compatibility_patch", name="worker thread reuse",
         source="src/rt/sched.c:sched_create_thread/tcb_by_entry",
         reason="a new sceKernelCreateThread for main_RunGameLoop's entry point reuses an "
                "existing dormant TCB instead of creating a fresh thread; opt-out via "
                "SR_NO_THREAD_REUSE=1",
         test="none"),
    dict(address=0x0029a174, category="temporary_compatibility_patch", name="launcher priority demotion",
         source="src/rt/sched.c:sched_create_thread_finish",
         reason="the launcher thread's declared priority is overridden to 50 (below the "
                "worker's ~38-40) to prevent it starving the worker via an unconditional "
                "SR_YIELD-heavy loop; opt-out via SR_NO_LAUNCHER_DEMOTE=1. Flagged in "
                "ISSUES.md P2 as the leading suspect for movement-triggered rendering "
                "glitches -- capture with SR_THLOG=1 SR_GELOG=1 before changing "
                "(SR_ROTLOG was retired with pick_next's rotation, 2026-07-18).",
         test="none"),
    dict(address=0x00292fa0, category="temporary_compatibility_patch", name="callback-list walker terminal miss",
         source="src/rt/recomp.c:dispatch (target==UINT32_MAX && s->pc==0x00292fa0 && ra==0x00047a0c)",
         reason="a circular callback-list walker's -1 terminal target is reported as complete "
                "for this exact inner call site only, instead of being treated as a permissive "
                "miss that would loop the outer walker forever",
         test="none"),
]

OVERRIDES = (
    GUEST_PATCHES + CODEGEN_CUSTOM_STUBS + HST_SIMPLE_STUBS +
    DISPATCH_HOOKS + DISPATCH_RANGE_HOOKS + SCHEDULER_HOOKS
)

# --- purely diagnostic hook GROUPS -------------------------------------------
# Read-only, env-gated (default off), no control-flow or guest-memory writes.
# Not individually enumerated (there are dozens of one-line trace points across
# sched.c) and not covered by the automated completeness gate -- listed here
# for transparency. If any of these ever gains a side effect, move it into
# DISPATCH_HOOKS/SCHEDULER_HOOKS above with a real category.
DIAGNOSTIC_GROUPS = [
    dict(name="boot_diag_trace_points",
         source="src/rt/sched.c:boot_diag/sr_boot_probe (SR_BOOT_DIAG)",
         addresses=[0x001039d8, 0x0008250c, 0x00082530, 0x0003dfd0, 0x0003d828, 0x0003e050,
                    0x0019668c, 0x00014934, 0x0019357c, 0x00015fb4, 0x001026b8, 0x000705b0,
                    0x000705e4, 0x000160e8],
         reason="bounded, sampled fprintf probes of guest state at these PCs; never writes "
                "guest memory or CPU state"),
    dict(name="copyspin_backedges", source="src/rt/sched.c:sr_yield (SR_COPYSPIN)",
         addresses=[0x00025a50, 0x00025a5c, 0x00025abc, 0x00025ac8],
         reason="register dump at the cache-flush/plane-copy emulator's loop back-edges"),
    dict(name="heapspin_probe_points", source="src/rt/sched.c:sr_yield (SR_HEAPSPIN)",
         addresses=[0x000115e8, 0x0000d62c, 0x00000c5c, 0x00048c18],
         reason="one-shot free-list/bin dump when the worker is spinning in the allocator"),
    dict(name="sched_spin_dump_points", source="src/rt/sched.c:sched_run spin-diagnostic dump",
         addresses=[0x0006ea40, 0x00014dac, 0x0000095c],
         reason="extra register/memory context appended to the periodic spin-detector dump"),
    dict(name="audio_trace", source="src/rt/sched.c:audio_trace (unconditional, single fprintf)",
         addresses=[0x000872cc],
         reason="logs PlayStream call arguments; read-only"),
    dict(name="init_walker_diagnostics", source="src/rt/recomp.c:dispatch (INIT_WALKER_GUARD / INIT_ARRAY_WALK / WALKER_RET)",
         addresses=[0x00000f98, 0x00000fdc],
         reason="saves/restores r16 around a dispatch and logs the init-array walk; the "
                "save/restore is a no-op by construction (restores exactly what it saved)"),
]


def all_documented_addresses() -> "set[int]":
    """Every address this manifest claims to account for (individual entries only;
    DIAGNOSTIC_GROUPS and range hooks with address=None are excluded by design)."""
    result = set()
    for o in OVERRIDES + HST_ENTRY_ROLES:
        addr = o.get("address")
        if isinstance(addr, int):
            result.add(addr)
    return result
