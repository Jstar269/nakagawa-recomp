// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Opt-in stale translated-code tracking (TD-27): DETECTION plus a per-entry
// stale flag and a dispatch-side query. The dispatch redirect itself is NOT
// yet wired (see "Dispatch hook" below): until it lands, the HLE cache
// handlers stay fail-loud (abort on a firing check), so a stale AOT body can
// never silently run.
//
// Background. The production interpreter (src/rt/guest_interp.c) re-reads the
// guest instruction word per step (MEM_R32(pc)), while ahead-of-time
// translated bodies run whatever was translated at build time: sr_lookup()
// (src/rt/recomp.h) checks structural executable ownership only and provides
// no content guards. Cache maintenance (sceKernelIcacheInvalidateAll/Range,
// sceKernelDcacheWriteback*, the `cache` instruction) performs no
// re-validation (tools/codegen.py emits `(void)0` for `cache`; the HLE cache
// entries are success no-ops). A guest that overwrites code and jumps to it
// therefore runs stale bytes on the AOT tier while the interpreter would
// fetch the new ones.
//
// Gate. Everything here is controlled by the SR_STALE_DETECT environment
// variable (any non-empty value other than "0" enables). The gate reuses two
// existing conventions, cited here:
//   - the cached-getenv idiom of SR_DISPLOG (src/rt/recomp.c, dispatch path):
//     the flag is resolved once into a static int, so the disabled path is a
//     single predicted-false branch with zero guest-memory touches;
//   - the fail-loud convention of SR_BREAK_FATAL (src/rt/recomp.c,
//     sr_break) and sr_unimplemented: a firing check prints one
//     STALE_CODE_DETECT line to stderr naming the block address, the first
//     differing word, and expected vs actual, and the production caller
//     aborts. The pure check functions below return an error code instead of
//     aborting so selftests can assert both directions without forking.
//
// Translation-time words. The runtime keeps no pristine image: the pure API
// compares through a caller-supplied reader, so production passes live guest
// memory and tests pass synthetic arrays. Codegen records the expectations
// with tools/codegen.py --stale-detect (default off, byte-identical output),
// which emits one compact (address, entry word, word count, FNV-1a) record
// per translated primary-image function -- never raw code bodies. Extra
// modules are deliberately excluded: they are translated at build time but
// never copied into guest RAM, so their arena bytes cannot be compared
// without false-firing on the first check.
//
// Cost when off. sr_stale_check_range()/sr_stale_check_all() return 0
// immediately on a cached disabled flag before touching guest memory; the
// default codegen emits no hook calls at all. sr_stale_block_is_stale()
// likewise returns 0 on one cached disabled branch before touching the
// record tables, so an unwired-by-default dispatch stays exactly on its
// current path.
//
// Stale flags (invalidation half, TD-27). Every registered word/block record
// carries a stale flag. A check that examines a record SETS the flag when the
// live bytes mismatch and CLEARS it when they match again (code restored), so
// the flag always reflects the most recent comparison. Records a check could
// not examine keep their flag: out-of-range records, unreadable words (a
// skipped block never fires and never clears), malformed ranges, and the
// gate-disabled path. sr_stale_reset() drops all records including flags; a
// repeated registration installs a fresh expectation with the flag clear.
//
// Dispatch hook (REQUIRED before stale blocks may execute; NOT YET WIRED).
// When a check marks a block stale, the next dispatch to that block must run
// the guest bytes through the existing single-step interpreter instead of
// the registered AOT body, until return to the caller:
//
//   1. src/rt/recomp.c dispatch_try_with_boundary(), in the `if (fn)` hit
//      path after the reloc/kseg/late-import fixups and before `fn(s)`:
//      when sr_stale_block_is_stale(target) is true, run
//      sr_guest_interp_run_with_boundary(s, target, call_boundary, &fault)
//      (or sr_guest_interp_run(s, target, &fault) when call_boundary is
//      NULL) and return its result exactly like the existing miss path
//      does (AOT_HANDOFF/CALL_RETURN return; EXCEPTION/ERET clear flow and
//      recurse; any reject propagates to the fail-closed wrapper).
//   2. src/rt/guest_interp.c sr_guest_interp_run_internal(), in the tier
//      selection ahead of `dispatch(s, pc)`: enter the AOT body only when
//      `sr_lookup(pc) && !sr_stale_block_is_stale(pc)`, else keep
//      interpreting. Without (2), the interpreter started by (1) would hand
//      the same stale pc straight back to dispatch and recurse.
//
// The hook is deliberately deferred: wiring it adds a stale_code.c edge to
// the recomp.c/guest_interp.c link surface, which is locked in several
// places this mission may not move -- the headless gate-stub link
// (tools/test_native_gate_stub_link.py
// test_same_headless_runtime_link_inputs_have_no_stale_symbols links
// recomp.c+guest_interp.c with no stale TU), the hosted-CI inline-build
// source lock (tools/test_native_gate_stub_link.py
// TestInlineCiSelftestLinkSync pins the vfpu-interp-selftest recipe against
// .github/workflows/ci.yml), the Makefile/CI/python-harness link audits
// (tools/test_title_runtime_config.py
// test_every_recomp_runtime_surface_links_guest_interp,
// tools/test_cpu_lle.py, tools/test_domain_mode.py), and the selftest
// recipes that would each need the new TU (heap, profiler, vfpu-interp,
// cpu-lle, domain-mode, dispatch-isolation). Until the hook lands, the HLE
// handlers keep the fail-loud abort, so detection without redirect still
// fails closed instead of running stale bytes.

#ifndef NAKAGAWA_STALE_CODE_H
#define NAKAGAWA_STALE_CODE_H

#include <stddef.h>
#include <stdint.h>

#define SR_STALE_DETECT_ENV "SR_STALE_DETECT"

/* Guest-word reader supplied by the caller. Returns 1 with *word_out set on
 * a readable word, 0 when the address is not comparable (unbacked span,
 * out of range). An unreadable word is SKIPPED, never treated as stale:
 * bytes the guest could not have overwritten through guest memory cannot
 * be stale translations of it. */
typedef int (*SrStaleReadFn)(uint32_t addr, uint32_t *word_out, void *ctx);

/* Forget all registered expectations. Does not touch the cached gate. */
void sr_stale_reset(void);

/* Record the translation-time word at a 4-aligned guest address. A repeated
 * registration replaces the expectation. Misaligned addresses and the zero
 * word-count block below are ignored: executable words are 4-aligned, and a
 * record that can never match a fetch must not be able to fire. */
void sr_stale_register_word(uint32_t addr, uint32_t expected_word);

/* Record a contiguous translated run: nwords guest words starting at the
 * 4-aligned block address addr, with expected_hash = FNV-1a over those words
 * in address order (see sr_stale_fnv1a). A repeated registration replaces the
 * record. */
void sr_stale_register_block(uint32_t addr, uint32_t nwords, uint32_t expected_hash);

/* Number of registered records (words plus blocks). */
uint32_t sr_stale_entry_count(void);

/* 1 when SR_STALE_DETECT opts in, else 0. Resolved once and cached (the
 * SR_DISPLOG idiom); the disabled path costs one predictable branch. */
int sr_stale_enabled(void);

/* FNV-1a over 32-bit words, little-endian byte order. The codegen helper
 * tools/codegen.py::stale_fnv1a implements the same function; both sides pin
 * the shared vectors (empty -> 0x811c9dc5, [0x00000000] -> 0x4b95f515,
 * [0x00000061] -> 0xf5e1d3e4). A NULL word pointer hashes as the empty
 * sequence. */
uint32_t sr_stale_fnv1a(const uint32_t *words, uint32_t nwords);

/* Compare the registered records overlapping [addr, addr+size) against the
 * reader. Words are checked first (precise word-level report), then blocks
 * (hash-level report). Every examined record updates its stale flag (set on
 * mismatch, cleared on match); skipped records keep theirs. Returns 1 when
 * any examined record mismatched -- after printing one STALE_CODE_DETECT
 * line for the first mismatch in check order -- else 0. A return of 0 also
 * covers every non-firing case, each deliberately silent: gate disabled, no
 * overlapping record, empty (size 0) or wrapping range, NULL reader, and
 * records whose bytes are currently unreadable. Output parameters are set
 * only on a firing return (word case: block and off name the differing word,
 * expected/actual are its values; block case: bad names the block and
 * expected/actual carry the hashes); they are zeroed otherwise. */
int sr_stale_check_range(uint32_t addr, uint32_t size, SrStaleReadFn read, void *ctx,
                         uint32_t *bad_addr_out, uint32_t *expected_out, uint32_t *actual_out);

/* Check every registered record, as for an invalidate-all. Same contract. */
int sr_stale_check_all(SrStaleReadFn read, void *ctx,
                       uint32_t *bad_addr_out, uint32_t *expected_out, uint32_t *actual_out);

/* Dispatch-side query for the hook documented above: 1 when the gate is
 * enabled and the block owning addr is currently flagged stale, else 0.
 * "Owning" covers the exact word record at addr, any block record whose
 * [addr, addr+nwords*4) span contains addr, and -- for a dispatch to a block
 * head -- any stale word record inside that head's span (the AOT body would
 * still execute it). Unregistered addresses, misaligned addresses, and the
 * gate-disabled path all read 0 after a single cached-flag branch. */
int sr_stale_block_is_stale(uint32_t addr);

/* `cache`-op hook emitted by codegen behind --stale-detect. Defined in
 * src/rt/hle.c (the TU that owns guest-memory access for cache syscalls);
 * a no-op returning immediately unless SR_STALE_DETECT is set, aborting on a
 * firing check like the HLE invalidate handlers. */
void sr_stale_note_cache_op(uint32_t addr);

#endif
