# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Title #2 Slice 1: profile=none must never inherit HST-specific codegen.

This test is the failing-before proof for the isolation invariant.

It builds a synthetic ET_EXEC whose entry 0x00010738 fans out via 35 jal
targets to every HST numeric guest address that currently carries title-
specific translation (NULL_BASE_WORD_LOADS, GUEST_PATCHES, boot probes,
MEMSET/ARRSHIFT fastpaths, GUEST_ABORT, and the 18 custom stubs in main()).

Under --profile=none the emitted C must be faithful: no sr_newlib_*,
no memcpy/memset native helpers, no MEMSET/ARRSHIFT fastpaths, no
GUEST_ABORT/_Exit(9), no sr_boot_probe, no bypass-loop inject.

Under --profile=hst the same instructions must reproduce the legacy
HST-specific behaviours (the equivalence half). This is proven here on a
synthetic specimen, because no private EBOOT.elf is available on this
machine.

Resume/owner pairs (HST_RESUME_OWNERS) are crafted so that
--profile=hst does not crash during entry_frame_balance verification:
each owner (0x30fdc,0x21ac0,0xb237c) is a callable frame that reaches its
resume (0x310b0,0x21c78,0xb26a0) at depth 0x20.

If this test passes after gating, it proves leak count 0. Before gating
on current main it must FAIL for profile=none.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import codegen  # noqa: E402


def _jal(target: int) -> int:
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def _beq_to(target: int, pc: int) -> int:
    # beq r0,r0, offset  where offset = (target - (pc+4))>>2
    off = (target - (pc + 4)) >> 2
    return (4 << 26) | (0 << 21) | (0 << 16) | (off & 0xFFFF)


def _addiu_sp_imm(imm: int) -> int:
    return (0x09 << 26) | (29 << 21) | (29 << 16) | (imm & 0xFFFF)


JR_RA = 0x03E00008
NOP = 0x00000000
LW_A0_T0 = 0x8C880000  # lw t0,0(a0)  -- NULL_BASE sentinel

# All HST numeric addresses that must be gated (35 unconditional + 2 diag-probe).
# Kept as constants so a new emission point that forgets its gate is caught here.
HST_BOOT_PROBES = (0x0003D828, 0x0003DFD0, 0x000705B0, 0x001026B8, 0x001039D8)
HST_PROBES = (0x000160E8, 0x000705E4)
HST_DIAG = (0x0010433C, 0x0006E9BC)
HST_FASTPATHS = (0x0006EA1C, 0x00108630)
HST_GUEST_PATCHES = (0x00010950, 0x00048320, 0x0004CDC8)
HST_NULL_BASE = (0x0003E014, 0x0003E04C, 0x0003E060, 0x000705D4)
HST_ABORT = 0x00000A1C
# 18 custom stubs in main() (excluding the entry itself which is the caller)
HST_CUSTOM_STUBS = (
    0x000011B0,
    0x0000260C,
    0x0000FE3C,
    0x0000F538,
    0x000101C4,
    0x00013524,
    0x00015EA0,
    0x000143B0,
    0x00046D14,
    0x0001034C,
    0x000468C8,
    0x001D9EB0,
    0x00011090,
    0x000110DC,
    0x000114A8,
    0x000114C0,
    0x000149A8,
)

# Resume/owner pairs that profile=hst verifies. Must be present in the image so
# the hst build does not fail with RESUME ROLE NOT CONFIRMED / OWNER DOES NOT
# COVER.
HST_RESUME_OWNERS = {
    0x000310B0: 0x00030FDC,
    0x00021C78: 0x00021AC0,
    0x000B26A0: 0x000B237C,
}


def _write_sparse_elf(path: Path, words: dict[int, int], entry: int) -> None:
    """One PT_LOAD covering 0..max(words)+8 with those words, entry=entry."""
    max_addr = max(words) if words else entry
    # Round up to incorporate final instruction's delay slot
    filesz = max_addr + 8  # 0-based
    if filesz < 0x1000:
        filesz = 0x1000
    payload_off = 52 + 32
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    # e_type=2 ET_EXEC, e_machine=8 MIPS, e_entry=entry
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        blob,
        16,
        2,
        8,
        1,
        entry,
        52,
        0,
        0,
        52,
        32,
        1,
        0,
        0,
        0,
    )
    # PT_LOAD r-x, vaddr 0
    struct.pack_into("<8I", blob, 52, 1, payload_off, 0, 0, filesz, filesz, 5, 4)
    for addr, word in words.items():
        blob[payload_off + addr] = word & 0xFF
        blob[payload_off + addr + 1] = (word >> 8) & 0xFF
        blob[payload_off + addr + 2] = (word >> 16) & 0xFF
        blob[payload_off + addr + 3] = (word >> 24) & 0xFF
    path.write_bytes(blob)


def _build_specimen_words() -> dict[int, int]:
    w: dict[int, int] = {}

    # --- entry that fans out to every HST address ---
    # Entry itself is 0x10738 (a malloc-class custom stub in hst). Its body is
    # jal fan-out + jr ra; under hst it will be stubbed, but analyzer discovery
    # of its jal targets happens regardless of stubbing.
    entry_base = 0x00010738
    targets: list[int] = []
    targets.append(HST_ABORT)
    targets.extend(HST_CUSTOM_STUBS)            # 17
    targets.extend(HST_GUEST_PATCHES)           # 3  -> now ~21
    targets.extend(HST_BOOT_PROBES)            # 5  -> 26
    targets.extend(HST_PROBES)                 # 2  -> 28
    targets.extend(HST_DIAG)                   # 2  -> 30
    targets.extend(HST_FASTPATHS)              # 2  -> 32
    targets.append(0x0003E014)                 # representative NULL_BASE (the others are separate funcs)
    # Also add remaining NULL_BASE as independent functions (not via jal) – they are
    # not needed as entry callees for discovery, but we will emit them as isolated
    # functions below. So jal list stays 35ish? We already have 33; add owners to reach 35+.
    # Add resume owners so they are discovered:
    targets.extend([0x00030FDC, 0x00021AC0, 0x000B237C])
    # That's 36 jal targets; entry body = 36*8 + 8 = 296 bytes. Fine.
    # For exact 35 we can drop one boot probe duplication etc; the global leak check
    # covers whatever remains, so count is not load-bearing. We use 36.
    for i, tgt in enumerate(targets):
        pc = entry_base + i * 8
        w[pc] = _jal(tgt)
        w[pc + 4] = NOP
    # final jr ra
    final = entry_base + len(targets) * 8
    w[final] = JR_RA
    w[final + 4] = NOP

    # --- provide faithful bodies for each HST function address ---
    # Most get a trivial jr ra body; the few that need a specific opcode get that opcode.
    def put_jr(addr: int) -> None:
        if addr not in w:
            w[addr] = JR_RA
            w[addr + 4] = NOP

    # NULL_BASE sentinels: lw at the HST address, then return
    for addr in HST_NULL_BASE:
        if addr == 0x0003E014:
            continue  # handled above? actually target list already includes it as jal target;
                     # but we need lw, not jal. So overwrite entry at that addr:
        w[addr] = LW_A0_T0
        w[addr + 4] = JR_RA
        w[addr + 8] = NOP
    # Overwrite the 0x3E014 jal entry? No, 0x3E014 was a jal target, its body is at 0x3E014 itself,
    # not at entry. The entry's jal table is at 0x10738.. ; 0x3E014's body is separate.
    w[0x0003E014] = LW_A0_T0
    w[0x0003E014 + 4] = JR_RA
    w[0x0003E014 + 8] = NOP
    # The other NULL_BASE bodies already placed above via loop
    # Ensure they are not overwritten by generic put_jr
    for addr in HST_NULL_BASE:
        if addr not in w:
            w[addr] = LW_A0_T0
            w[addr + 4] = JR_RA
            w[addr + 8] = NOP

    # GUEST_PATCHES: branch at the HST address
    # 0x10950 : beq r0,r0,+1 ; nop ; jr ra at target
    w[0x00010950] = _beq_to(0x00010958, 0x00010950)
    w[0x00010954] = NOP
    w[0x00010958] = JR_RA
    w[0x0001095C] = NOP
    # 0x4cdc8 : same pattern but target 0x4cdd0
    w[0x0004CDC8] = _beq_to(0x0004CDD0, 0x0004CDC8)
    w[0x0004CDCC] = NOP
    w[0x0004CDD0] = JR_RA
    w[0x0004CDD4] = NOP
    # 0x48320 : arbitrary branch (inject patch) – give beq as well
    w[0x00048320] = _beq_to(0x00048328, 0x00048320)
    w[0x00048324] = NOP
    w[0x00048328] = JR_RA
    w[0x0004832C] = NOP

    # Remaining HST addresses that are pure function starts: give jr ra
    # (skip those already handled as branch/lw Owners/Resumes)
    for addr in list(HST_BOOT_PROBES) + list(HST_PROBES) + list(HST_DIAG) + list(HST_FASTPATHS):
        put_jr(addr)
    for addr in HST_CUSTOM_STUBS:
        put_jr(addr)
    put_jr(HST_ABORT)
    # Also simple-stub representative: include one to prove simple-stub gating
    # (already gated via SIMPLE_STUBS). Use 0x15f98 as representative.
    put_jr(0x00015F98)

    # The three HST_MANUAL_CALLABLES that are executable leafs – give them jr bodies
    # so the hst output for those is tidy.
    for addr in (0x00014430, 0x0003DB3C, 0x00042998, 0x0005A648):
        put_jr(addr)

    # --- Resume/owner crafting ---
    # Owner: addiu sp,-32; nop; beq -> resume
    # Resume: addiu sp,+32; nop; jr ra; nop
    for resume, owner in HST_RESUME_OWNERS.items():
        # Owner body
        w[owner] = _addiu_sp_imm(0xFFE0)  # -32
        w[owner + 4] = NOP
        w[owner + 8] = _beq_to(resume, owner + 8)
        w[owner + 12] = NOP
        # Resume body
        w[resume] = _addiu_sp_imm(0x0020)  # +32
        w[resume + 4] = NOP
        w[resume + 8] = JR_RA
        w[resume + 12] = NOP

    return w


class ProfileIsolationTests(unittest.TestCase):
    """HST does not leak into profile=none; profile=hst reproduces legacy behaviours."""

    def _run_codegen(self, profile: str) -> str:
        words = _build_specimen_words()
        with tempfile.TemporaryDirectory(prefix="nakagawa-profile-isolation-") as tmp:
            tmp_path = Path(tmp)
            elf_path = tmp_path / "specimen.elf"
            _write_sparse_elf(elf_path, words, entry=0x00010738)
            out_c = tmp_path / "out.c"
            # In-process call to codegen.main (avoids subprocess overhead + keeps
            # coverage of the profile threading path).
            argv = [
                "codegen.py",
                str(elf_path),
                str(out_c),
                "--base=0",
                f"--profile={profile}",
                "--funcs-per-chunk=2000",
            ]
            orig_stderr = sys.stderr
            orig_stdout = sys.stdout
            # Silence codegen's noisy prints/stderr for clean unittest output, but
            # preserve failure diagnostics if main returns non-zero.
            try:
                from io import StringIO

                buf_out = StringIO()
                buf_err = StringIO()
                sys.stdout = buf_out  # type: ignore[assignment]
                sys.stderr = buf_err  # type: ignore[assignment]
                rc = codegen.main(argv)
                captured_out = buf_out.getvalue()
                captured_err = buf_err.getvalue()
            finally:
                sys.stdout = orig_stdout  # type: ignore[assignment]
                sys.stderr = orig_stderr  # type: ignore[assignment]
            self.assertEqual(rc, 0, f"codegen failed profile={profile}: {captured_out} {captured_err}")
            # Collect all emitted translation units (out.c + out_0.c ...). The driver
            # writes out_0.c etc in the same directory as out_c, sharing its basename.
            texts: list[str] = []
            if out_c.is_file():
                texts.append(out_c.read_text(encoding="ascii", errors="ignore"))
            for chunk in sorted(tmp_path.glob("out_*.c")):
                texts.append(chunk.read_text(encoding="ascii", errors="ignore"))
            combined = "\n".join(texts)
            # Also include funcs header for symbol presence (not needed for leak strings)
            funcs_h = tmp_path / "out_funcs.h"
            if funcs_h.is_file():
                combined += "\n" + funcs_h.read_text(encoding="ascii", errors="ignore")
            self.assertTrue(combined.strip(), f"empty codegen output for profile={profile}")
            return combined

    def test_profile_none_has_no_hst_specific_emission(self):
        combined = self._run_codegen("none")
        # Global invariant: none of these HST-bound substrings may appear in a
        # profile=none translation (each occupies an HST numeric address).
        leaks: list[str] = []
        # Custom allocator bridges
        for needle in (
            "sr_newlib_malloc",
            "sr_newlib_free",
            "sr_newlib_memalign",
            "sr_newlib_realloc",
            "sr_guest_sprintf",
            "sr_newlib_",
        ):
            if needle in combined:
                leaks.append(needle)
        # Bulk native helpers / fastpaths
        for needle in (
            "MEMSET_FASTPATH",
            "ARRSHIFT_FASTPATH",
            "memset(SR_HOST",
            "memmove(SR_HOST",
            "sr_guest_span",
            "sr_guest_span_writable",
        ):
            if needle in combined:
                leaks.append(needle)
        # Abort / boot probes / patch injects
        for needle in (
            "GUEST_ABORT",
            "_Exit(9)",
            "sr_boot_probe",
            "bypass loop",
            "s->r[16] = s->r[3];",
        ):
            if needle in combined:
                leaks.append(needle)
        # Simple-stub representative should not be a custom ``return static val`` under none;
        # faithful translation will contain MEM_R / jr handling, not the stub phrase.
        # The stub phrase is "custom stub: Config_LoadGameSettings" etc. Check absence:
        if "custom stub: Config_LoadGameSettings" in combined:
            leaks.append("custom stub: Config_LoadGameSettings")
        self.assertEqual(leaks, [], f"profile=none leaked HST-specific text: {leaks}")

        # Also per-site faithfulness: a NULL_BASE load must be plain MEM_R32, not guarded.
        # The specimen contains lw at 0x3E014. Faithful: MEM_R32(s->r[...]) without "== 0u ? 0u".
        # Under hst it is guarded. Here assert faithful (no guard) for none.
        # Find the slice for f_0003e014 if present; otherwise assert globally that the guard
        # does not appear.
        self.assertNotIn("== 0u ? 0u : MEM_R32", combined)

    def test_profile_hst_preserves_legacy_hst_behaviours(self):
        combined = self._run_codegen("hst")
        # Each class must still emit its legacy behaviour under hst.
        must: list[str] = []
        def need(sub: str) -> None:
            if sub not in combined:
                must.append(sub)

        need("sr_newlib_malloc(s->r[5], owner_ra)")
        need("sr_newlib_free(s->r[5], owner_ra)")
        need("sr_newlib_memalign")
        need("sr_newlib_realloc")
        need("sr_guest_sprintf(s);")
        need("memset(SR_HOST")
        need("memmove(SR_HOST")
        need("MEMSET_FASTPATH")  # comment / log line from fastpath
        need("ARRSHIFT_FASTPATH")
        need("GUEST_ABORT")
        need("_Exit(9)")
        # At least one boot probe (0x3d828 is in the fan-out)
        need("sr_boot_probe(s, 0x0003d828u);")
        need("sr_boot_probe(s, 0x000160e8u);")
        need("sr_boot_probe(s, 0x000705e4u);")
        need("bypass loop 0x10950")
        # Inject for 0x10950
        need("s->r[16] = s->r[3];")
        # Force for 0x48320
        need("_c = 1u;")
        # NULL_BASE guarded form
        need("== 0u ? 0u : MEM_R32")
        self.assertEqual(must, [], f"profile=hst missing legacy HST text: {must}")


if __name__ == "__main__":
    unittest.main()
