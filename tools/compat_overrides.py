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
OVERRIDES against the authoritative, mechanically-extractable sources of hooks
in the tree:
  - tools/codegen.py: GUEST_PATCHES, HST entry-role metadata,
    host_stubs.HST_SIMPLE_STUBS, and the
    per-address custom stubs between the "--- CUSTOM STUBS START/END ---"
    markers in emit_function's driver loop.
  - src/rt/recomp.c: the g_exact_hooks[]/g_range_hooks[] DispatchHook tables.
  - src/rt/hle.c: guest addresses used as addresses (MEM_R*/MEM_W*/dispatch/
    ge_call_guest*), grouped in HLE_GUEST_ADDRESS_GROUPS (added 2026-08-20;
    before that, hle.c was in neither the automatic extraction nor the manual
    groups, so its title addresses sat outside this inventory entirely).
CI fails if any source contains an address this manifest does not list, or
if this manifest lists an address the source no longer contains (stale entry).

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

#: Title-2 readiness census buckets (docs/PORTING.md "Title coupling in the
#: generic core").  Every HLE_GUEST_ADDRESS_GROUPS group carries exactly one.
#: GENERIC_PSP_SEMANTIC       -- behavior is a generic PSP fact, not title
#:                               knowledge; no profile needed.
#: PROFILE_OWNED_CONFIGURATION -- a per-title value that belongs in the title
#:                               manifest/profile, not in generic core.
#: EXPLICIT_COMPATIBILITY_OVERRIDE -- semantic debt: title-specific behavior
#:                               in generic core; must answer the five
#:                               questions (why/scope/fallback/evidence/
#:                               accidental inheritance) in the group dict.
#: DIAGNOSTIC_ONLY            -- read-only, no control-flow or guest-memory
#:                               side effects.
#: PRIVATE_ACCEPTANCE_ONLY    -- exists only to pass a private acceptance
#:                               route; no public evidence.
#: FALSE_POSITIVE             -- a shape that looks like a coupling but is not
#:                               (verified, not assumed).
#: UNRESOLVED_COUPLING        -- real title coupling with no classification;
#:                               must fail the gate, never pass silently.
TITLE2_BUCKETS = {
    "GENERIC_PSP_SEMANTIC",
    "PROFILE_OWNED_CONFIGURATION",
    "EXPLICIT_COMPATIBILITY_OVERRIDE",
    "DIAGNOSTIC_ONLY",
    "PRIVATE_ACCEPTANCE_ONLY",
    "FALSE_POSITIVE",
    "UNRESOLVED_COUPLING",
}

#: Narrow, explicit, reviewable SITE rules that keep a generic PSP fact out of
#: the title-coupling scan.  This REPLACES both the original blanket numeric
#: ceiling (HLE_GUEST_ADDRESS_CEILING == 0x04000000) and the later whole-region
#: rule (HLE_GENERIC_ADDRESS_RULES: psp_vram_window 0x04000000..0x041fffff),
#: which silently classified ANY access into VRAM as generic -- including a
#: hypothetical title-specific MEM_W32(0x04012340u, v).  A whole-region
#: exemption is a blind spot by construction: VRAM is hardware geometry, but
#: an absolute guest address used through MEM_R*/MEM_W* is a guest location
#: the runtime knows about by number, and a title-specific write into VRAM is
#: just as much title coupling as one into RAM.
#:
#: An address is generic ONLY when an exact site rule here covers it: a rule
#: names the enclosing hle.c function, the call/return shape, and the exact
#: literal.  Rules must be narrow (an architectural constant returned by one
#: named handler, never "everything above X" and never a whole hardware
#: window), must state the generic PSP fact they stand for, and must never
#: exempt a memory-access shape (MEM_R*/MEM_W*, sr_r32/sr_w32, dispatch,
#: ge_call_guest*): a direct fixed MEM_R/MEM_W at an arbitrary VRAM address is
#: exactly the shape a title-specific coupling takes and must always be
#: inventoried.  test_compat_manifest.py enforces those properties
#: mechanically.
HLE_GENERIC_SITE_RULES = [
    dict(name="edram_base_return", function="h_GeEdramGetAddr", shape="return",
         address=0x04000000,
         reason="sceGeEdramGetAddr returns the architectural EDRAM base "
                "0x04000000 on every PSP; it is a hardware constant, not "
                "title knowledge.  The exemption is this one return site only "
                "-- a MEM_R/MEM_W at 0x04000000 is still inventoried."),
]


def is_generic_site(function: str, shape: str, address: int) -> bool:
    """True only for a site covered by an explicit generic-site rule.

    A rule matches when the enclosing function, the call/return shape and the
    exact literal all agree.  This is the only sanctioned way an absolute
    guest address escapes the title-coupling scan.  There is deliberately no
    fallback ceiling and no whole-region exemption: a site no rule covers is
    scanned no matter how high its address is.
    """
    for rule in HLE_GENERIC_SITE_RULES:
        if (rule["function"] == function and rule["shape"] == shape
                and rule["address"] == address):
            return True
    return False

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
         reason="a new sceKernelCreateThread for the configured worker entry reuses an "
                "existing dormant TCB instead of creating a fresh thread; opt-out via "
                "SR_NO_THREAD_REUSE=1. The address above is no longer compiled into the "
                "runtime: it reaches sched.c only as the runtime_bindings.worker_thread_entry "
                "of a validated title manifest, so an unconfigured build never applies this "
                "override at all. Retire by proving the guest's create/exit sequence needs no "
                "reuse, which retires the binding with it.",
         test="make sched-selftest (generic/fixture-a/fixture-b matrix)"),
    dict(address=0x0029a174, category="temporary_compatibility_patch", name="launcher priority demotion",
         source="src/rt/sched.c:sched_create_thread_finish",
         reason="the launcher thread's declared priority is overridden to 50 (below the "
                "worker's ~38-40) to prevent it starving the worker via an unconditional "
                "SR_YIELD-heavy loop; opt-out via SR_NO_LAUNCHER_DEMOTE=1. Flagged in "
                "ISSUES.md P2 as the leading suspect for movement-triggered rendering "
                "glitches -- capture with SR_THLOG=1 SR_GELOG=1 before changing "
                "(SR_ROTLOG was retired with pick_next's rotation, 2026-07-18). The address "
                "above is no longer compiled into the runtime: it reaches sched.c only as the "
                "runtime_bindings.launcher_thread_entry of a validated title manifest, so an "
                "unconfigured build never demotes anything. Retire by fixing the underlying "
                "scheduling inversion, which retires the binding with it.",
         test="make sched-selftest (generic/fixture-a/fixture-b matrix)"),
    dict(address=0x000468c8, category="temporary_compatibility_patch", name="worker relaunch trampoline",
         source="src/rt/sched.c:deliver_vblank",
         reason="a DORMANT thread holding the WORKER role is restarted on the next VBLANK, "
                "as a host surrogate for the GE list-complete callback that re-arms the "
                "guest's frame loop on real hardware; opt-out via SR_NO_RELAUNCH=1. Reached "
                "only through the captured worker role, so a build with no worker binding "
                "re-arms nothing. Retire by implementing the real list-complete callback "
                "path, which retires the surrogate.",
         test="make sched-selftest (generic build asserts no role is captured, so no relaunch)"),
    dict(address=0x0029a174, category="temporary_compatibility_patch", name="launcher reent ownership",
         source="src/rt/sched.c:register_libc_thread/init_guest_reent",
         reason="a thread holding the LAUNCHER role seeds g_master_reent for every later "
                "thread, keeps its own independently-initialized reent instead of inheriting "
                "the master's, and is skipped by the guest reent-hash pre-registration "
                "because the guest's own registration function runs from the launcher entry "
                "and must find an empty slot. Until 2026-08-20 all three were UID-number "
                "tests against a role global that defaulted to the historical allocation "
                "0x111, so in a build with NO launcher binding an ordinary thread allocated "
                "0x111 inherited every one of them. They are role tests now: roles start at "
                "SR_ROLE_UID_NONE and are captured only from a configured entry, so an "
                "unconfigured build applies none of this. Retire together with the launcher "
                "binding once the guest's reent bring-up needs no host participation.",
         test="make sched-selftest (generic build allocates UID 0x111 and asserts it is ordinary)"),
    dict(address=0x002cf338, category="temporary_compatibility_patch", name="master reent fallback address",
         source="src/rt/sched.c:g_master_reent initializer",
         reason="g_master_reent starts at a title guest address so threads created before "
                "any launcher registration have a reent to inherit. It is guarded by an "
                "in-range and non-zero-content check, so it is inert in a build without that "
                "title's data, but it is still a title guest address compiled into generic "
                "runtime code and is NOT covered by the runtime_bindings surface. Retire by "
                "making the pre-registration window explicit (or by binding it) rather than "
                "by deleting the default, which would silently change the inheritance of "
                "every thread created before the launcher registers.",
         test="none"),
]

# --- src/rt/recomp.c: dispatch bindings owned by TITLE CONFIGURATION ----------
# These three used to be numeric literals compiled into generic dispatch. They are
# now typed entries in a validated title manifest's runtime_bindings block
# (dispatch_aliases / callback_terminators), so the addresses below no longer exist
# anywhere in generic runtime code: they reach dispatch() only as configuration, and
# an unconfigured build applies none of them.
#
# They stay inventoried because the SEMANTIC DEBT did not go away -- moving an
# override behind configuration makes it honest, not absent. Each entry names the
# generic mechanism, the executable test that proves it acts only where configured,
# and what would retire the binding for good.
TITLE_CONFIGURED_DISPATCH = [
    dict(address=0x00030950, category="faithful_abi_bridge", name="TC30950",
         source="src/rt/recomp.c:dispatch (runtime_bindings.dispatch_aliases)",
         reason="a tail-call target lands at a callee's +8 entry point (past its prologue) "
                "that codegen does not separately register; the configured alias enters the "
                "real registered entry instead, with an equivalent net stack delta. The "
                "mechanism is generic (enter a registered body); only the address pair is "
                "configured, and an unconfigured build treats this address as an ordinary "
                "dispatch miss. Retire by registering the mid-function entry point in "
                "codegen, which removes the need for any alias.",
         test="make dispatch-isolation-selftest (generic/fixture-a/fixture-b matrix)"),
    dict(address=0x0003e06c, category="temporary_compatibility_patch",
         name="callback-list walker null terminator",
         source="src/rt/recomp.c:dispatch (runtime_bindings.callback_terminators)",
         reason="a circular callback-list walker reaches its terminal entry with target 0; "
                "reported as COMPLETE at this exact return site instead of as a permissive "
                "miss, which the walker reads as \"continue\" and loops on forever. Was an "
                "uninventoried literal in generic dispatch until 2026-08-21. Retire by "
                "modelling the guest's list terminator faithfully so the walk ends on its "
                "own. This site constrains ra only, which is the constraint the original "
                "hardcoded check applied; narrowing it to a pc as well would be a behavior "
                "change, not a migration.",
         test="make dispatch-isolation-selftest (generic/fixture-a/fixture-b matrix)"),
    dict(address=0x00292fa0, category="temporary_compatibility_patch",
         name="callback-list walker terminal miss",
         source="src/rt/recomp.c:dispatch (runtime_bindings.callback_terminators)",
         reason="the same walker's -1 terminal target, reported as complete for this exact "
                "inner call site (pc plus ra) only. Retire together with the null terminator "
                "above: both are surrogates for a faithfully-modelled list end.",
         test="make dispatch-isolation-selftest (generic/fixture-a/fixture-b matrix)"),
]

OVERRIDES = (
    GUEST_PATCHES + CODEGEN_CUSTOM_STUBS + HST_SIMPLE_STUBS +
    DISPATCH_HOOKS + DISPATCH_RANGE_HOOKS + SCHEDULER_HOOKS +
    TITLE_CONFIGURED_DISPATCH
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


# --- src/rt/hle.c: guest addresses used as addresses ------------------------
#
# Until 2026-08-20 this manifest's mechanically-checked sources were
# tools/codegen.py and src/rt/recomp.c only, and the manually-maintained groups
# above covered src/rt/sched.c.  src/rt/hle.c was in neither, so 38 distinct
# HST guest addresses across 50 sites -- including a complete guest
# display-driver initialisation sequence dispatched from a generic
# sceDisplaySetMode handler and a read-only umd.ufl head dump reached through
# a cast-wrapped MEM_R8((uint32_t)(...)) shape -- were entirely outside the
# semantic-debt inventory.  They are listed here and cross-checked by
# tools/test_compat_manifest.py, so a new one cannot be added silently.
#
# The scan covers every absolute guest address and dispatch/call target
# regardless of magnitude.  A blanket numeric ceiling originally excluded
# everything >= 0x04000000; that blind spot is removed.  A later whole-region
# exemption for the VRAM window (0x04000000..0x041fffff) is also removed: a
# direct MEM_R/MEM_W at an arbitrary VRAM address must be inventoried, not
# silently classified generic.  Generic PSP constants are exempted only
# through the narrow site rules in HLE_GENERIC_SITE_RULES above (exact
# function + shape + literal), never through magnitude or a hardware window.
#
# A "site" is one literal guest address used as an address through one of the
# small, stable call shapes the extractor recognises (MEM_R*/MEM_W*, dispatch,
# ge_call_guest*, sr_r32/sr_w32, including cast-wrapped address bases such as
# MEM_R8((uint32_t)(0x... + off))).  An address used on multiple lines is one
# distinct address with multiple sites.
#
# `title2_bucket` uses the census vocabulary above.  For
# EXPLICIT_COMPATIBILITY_OVERRIDE groups, `generic_fallback`, `evidence`,
# `title_scope` and `accidental_inheritance` answer the five review questions;
# test_compat_manifest.py requires them.
#
# This inventories the coupling; it does not retire it.  The three
# `temporary_compatibility_patch` groups below are the ones that matter for
# running a second title: they read and write addresses that mean something
# only in this title's memory map, from handlers named after generic PSP APIs.
# A different guest executable reaching h_DisplaySetMode would dispatch to
# whatever happens to live at 0x00000bcc in ITS map.  Retiring them is tracked
# by issue #98 (compatibility-override surface) and the readiness record in
# docs/PORTING.md.  This previously cited #20, which is a merged pull request
# about sceSasCore routing -- so the surface had no tracker at all.
HLE_GUEST_ADDRESS_GROUPS = [
    dict(name="guest_bss_snapshots", category="diagnostic",
         title2_bucket="DIAGNOSTIC_ONLY",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:sr_capture_mainthread_diag / sr_dump_mainthread_diag / "
                "h_ExitThread re-snapshot / guest-printf hook / h_CpuSuspendIntr / "
                "h_CpuResumeIntr traces",
         addresses=[0x0030a000, 0x0030a004, 0x0030a008, 0x0030a00c,
                    0x0030a010, 0x0030a014, 0x0030a018, 0x0030a01c,
                    0x0030a020, 0x0030a024, 0x0030a028, 0x0030a02c,
                    0x0030a030, 0x0030a034, 0x0030a038, 0x0030a03c,
                    0x0030a040, 0x0030a044, 0x0030a048, 0x0030a04c,
                    0x0030a054, 0x0030a058, 0x0030a05c,
                    0x0030aa88,
                    0x0031a03c, 0x0031a040, 0x0031a044],
         reason="read-only MEM_R32 snapshots of this title's libc/module-registry and "
                "gp-relative frame-table bss, printed beside a thread-exit, guest-printf, "
                "or suspend/resume trace. No guest memory or CPU state is written, so on "
                "another title these print unrelated words rather than changing any decision.",
         generic_fallback="none needed -- the reads are diagnostic only and the printed "
                          "words are meaningless on another title, which is harmless",
         evidence="SOURCE_SHAPE: read-only usage verified at every site; no MEM_W* on "
                  "these addresses",
         accidental_inheritance="no -- diagnostic output only, no control-flow or write "
                                "side effects",
         test="none"),
    dict(name="exit_path_context", category="diagnostic",
         title2_bucket="DIAGNOSTIC_ONLY",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:h_ExitGame / h_ExitThread trailing context dump",
         addresses=[0x0310a034, 0x002cf6b4],
         reason="read-only context words appended to the exit traces. 0x0310a034 does not "
                "match the 0x0031a0xx frame-table block the surrounding code otherwise "
                "reads and looks like a transposed digit; it is inventoried as written "
                "rather than silently corrected, because a read-only diagnostic is the "
                "wrong place to guess.",
         generic_fallback="none needed -- read-only context words in an exit dump",
         evidence="SOURCE_SHAPE: read-only usage at lines 1475/1665; env-gated",
         accidental_inheritance="no -- diagnostic output only",
         test="none"),
    dict(name="umd_ufl_head_dump", category="diagnostic",
         title2_bucket="DIAGNOSTIC_ONLY",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:sr_umd_ufl_head_dump",
         addresses=[0x0030b8d0],
         reason="read-only MEM_R8 hex dump of the umd.ufl on-disk header block, reached "
                "through a cast-wrapped base MEM_R8((uint32_t)(0x0030b8d0u + off + i)) "
                "behind an SR_UMDDUMP env gate. It reads a diagnostic-only region and "
                "never writes guest memory.",
         generic_fallback="none needed -- env-gated diagnostic output only",
         evidence="SOURCE_SHAPE: read-only usage at lines 5554/5557 behind SR_UMDDUMP; "
                  "the cast-wrapped shape was previously invisible to the extractor",
         accidental_inheritance="no -- diagnostic output only",
         test="none"),
    dict(name="libfont_ready_flag", category="temporary_compatibility_patch",
         title2_bucket="EXPLICIT_COMPATIBILITY_OVERRIDE",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:h_LoadModule",
         addresses=[0x002d132c],
         reason="loading a path containing 'libfont.prx' writes 1 to a guest word at a "
                "fixed address. A generic module-load handler writing a title-specific "
                "global is title coupling: another executable loading a similarly named "
                "PRX would take an unrelated word to 1.",
         generic_fallback="h_LoadModule already returns a real UID and populates the "
                          "module registry without the write; the write is a title-pacing "
                          "assist only",
         evidence_tier="PRIVATE_ACCEPTANCE",
         evidence="private acceptance record; details retained outside public tree",
         accidental_inheritance="yes -- ANY executable that loads a path containing "
                                "'libfont.prx' gets an unrelated guest word forced to 1",
         test="none"),
    dict(name="frame_ready_latch_assist", category="temporary_compatibility_patch",
         title2_bucket="EXPLICIT_COMPATIBILITY_OVERRIDE",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:sr_vblank_tick / ge_finish_callback / ge_finish_latch_assist",
         addresses=[0x00331b80],
         reason="the runtime seeds a counter at a fixed guest address, decrements it when "
                "a display list completes with no registered guest finish callback, and "
                "after 30 vblanks with it stuck above zero forces it down. Forcing a "
                "guest latch is precisely the shape the project's no-band-aids rule "
                "names; it is inventoried here so it stays visible rather than being "
                "rediscovered.",
         generic_fallback="without the assist the guest render loop waits on a latch only "
                          "the guest's own finish callback can clear; the assist covers "
                          "lists with no registered callback",
         evidence_tier="PRIVATE_ACCEPTANCE",
         evidence="private acceptance record; details retained outside public tree",
         accidental_inheritance="yes -- any title whose render loop happens to read "
                                "0x00331b80 (or where that address is live memory) sees "
                                "HLE-driven writes",
         test="none"),
    dict(name="runtime_sync_callback_config", category="temporary_compatibility_patch",
         title2_bucket="EXPLICIT_COMPATIBILITY_OVERRIDE",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:ensure_runtime_sync_callbacks, reached from h_DisplaySetMode",
         addresses=[0x00333138, 0x002bdf38,
                    0x000823f0, 0x00082438,
                    0x00082474, 0x0008249c,
                    0x000824c0, 0x000824e8],
         reason="sceDisplaySetMode installs this title's runtime sync callbacks. It reads "
                "and writes an HST configuration block based at 0x00333138 (+0x0c sema "
                "handle, +0x30 mode, +0x34/+0x38 enter/leave, +0x4c0 initializer flag), "
                "may create an HLE semaphore whose NAME POINTER is handed to the guest as "
                "0x002bdf38, and then stores one of three pairs of guest wrapper entry "
                "points into that block. Every one of the eight is a location in HST's map "
                "and nothing else. This entry exists because none of them were visible to "
                "the coupling gate before 2026-08-21: not one is written inside a MEM_* "
                "call, so the direct-literal regex matched none of them while the census "
                "reported itself complete at 38/38.",
         generic_fallback="none today, and that is the point -- the handler has no path "
                          "that installs sync callbacks without these addresses. The "
                          "generic PSP semantic (register mode/width/height, drive vblank "
                          "cadence) does not require them; the bring-up replay does.",
         retirement="the eight values are PROFILE_OWNED_CONFIGURATION in shape: a config "
                    "base with a fixed field layout, a name pointer, and three MODE-KEYED "
                    "PAIRS of wrapper entries. They must NOT be flattened into scalar "
                    "runtime_bindings -- the pairing and the mode that selects it are part "
                    "of the meaning. The host-side replay and seeding behavior around them "
                    "stays EXPLICIT_COMPATIBILITY_OVERRIDE until it has a generic "
                    "mechanism to be configured INTO.",
         evidence_tier="SOURCE_SHAPE",
         evidence="SOURCE_SHAPE: the eight literals and their read/write offsets are read "
                  "out of src/rt/hle.c; h_DisplaySetMode is registered unconditionally in "
                  "hle_register_display_handlers(), with no title gate, so any guest "
                  "calling sceDisplaySetMode reaches this code. No route was run for this "
                  "inventory entry and none is claimed.",
         accidental_inheritance="yes, and this is the worst shape in the inventory: a "
                                "second executable that calls sceDisplaySetMode has "
                                "whatever lives at 0x00333138 in ITS map read AND WRITTEN, "
                                "gets 0x002bdf38 handed to sceKernelCreateSema as a name "
                                "pointer, and has two of the six wrapper addresses stored "
                                "where its own code will later call them.",
         test="tools/test_compat_manifest.py:HleIndirectCouplingGrammar"),
    dict(name="display_setmode_guest_init", category="temporary_compatibility_patch",
         title2_bucket="EXPLICIT_COMPATIBILITY_OVERRIDE",
         title_scope="hst-ucus98701",
         source="src/rt/hle.c:h_DisplaySetMode",
         addresses=[0x00000bcc, 0x0029a8bc, 0x0001dc00,
                    0x0031fcc0, 0x00311140, 0x002d0738],
         reason="sceDisplaySetMode replays a display-driver bring-up sequence: it "
                "calls guest functions at 0x00000bcc, 0x0029a8bc and 0x0001dc00, then "
                "forces the render-context magic at 0x0031fcc0 and seeds the "
                "render-command-table ready flag and context word. This is the single "
                "largest title dependency in the runtime and the clearest blocker to "
                "running a second executable: the addresses are dispatch targets, so a "
                "different guest would be called at whatever lives at those offsets in "
                "its own map.",
         generic_fallback="the handler's non-bring-up path is the generic PSP contract "
                          "(mode/width/height registration and vblank cadence); the guest "
                          "calls replace display-driver init the real title would do itself",
         evidence_tier="PRIVATE_ACCEPTANCE",
         evidence="private acceptance record; details retained outside public tree",
         accidental_inheritance="yes -- and worse than the other groups: three of the six "
                                "addresses are DISPATCH TARGETS, so a second executable "
                                "would execute whatever its own map holds at those offsets",
         test="none"),
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
