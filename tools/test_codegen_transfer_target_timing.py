# SPDX-License-Identifier: GPL-2.0-or-later

"""A computed transfer must read its target register AT the transfer.

MIPS executes a jump's delay slot AFTER the jump has already read its target
register, so a slot that writes that register cannot redirect control.  Generated
code used to read the register after emitting the slot, which silently redirected
`jalr`/`jr $rs` whenever the slot aliased the target -- for example the ordinary
`jr $t9` tail-call idiom preceded by a slot that reloads $t9.

The defect was found by the AOT/interpreter cosimulation gate
(``fixtures/cosim``, cell ``jrslot``), which executed the same source-owned guest
bytes both ways and reported the two lanes entering different callees.  These
tests pin the emission order so it cannot regress without an executable gate run.
"""

import unittest

import codegen


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


JR_RA = 0x03E00008
NOP = 0x00000000


def jr(rs):
    return (rs & 0x1F) << 21 | 0x08


def jalr(rd, rs):
    return (rs & 0x1F) << 21 | (rd & 0x1F) << 11 | 0x09


def addu(rd, rs, rt):
    return (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | 0x21


class TransferTargetTimingTests(unittest.TestCase):
    """The target read must precede the delay slot in the emitted statements."""

    def _emit(self, words, start=0x1000, end=0x1030, known=None):
        elf = FakeElf(words)
        known = known if known is not None else {start}
        return "\n".join(codegen.emit_function(elf, start, [(start, end)], known))

    def test_jalr_reads_target_before_its_delay_slot(self):
        # jalr t9, t0  /  addu t0, t1, zero   -- the slot aliases the target.
        text = self._emit(
            {
                0x1000: jalr(25, 8),
                0x1004: addu(8, 9, 0),
                0x1008: JR_RA,
                0x100C: NOP,
            }
        )
        capture = text.index("uint32_t _t = s->r[8];")
        slot = text.index("s->r[8] = (s->r[9] + 0u);")
        call = text.index("dispatch(s, _t);")
        self.assertLess(capture, slot, "jalr target captured after its delay slot")
        self.assertLess(slot, call, "delay slot emitted after the transfer")

    def test_jalr_link_is_written_before_its_delay_slot(self):
        # The link value is architecturally visible to the delay slot.
        text = self._emit(
            {
                0x1000: jalr(31, 8),
                0x1004: addu(8, 9, 0),
                0x1008: JR_RA,
                0x100C: NOP,
            }
        )
        link = text.index("s->r[31] = 0x00001008u;")
        slot = text.index("s->r[8] = (s->r[9] + 0u);")
        self.assertLess(link, slot, "jalr link written after its delay slot")

    def test_computed_jr_reads_target_before_its_delay_slot(self):
        # jr t9 / addu t9, t1, zero -- the classic reload-in-slot tail call.
        text = self._emit(
            {
                0x1000: jr(25),
                0x1004: addu(25, 9, 0),
                0x1008: JR_RA,
                0x100C: NOP,
            }
        )
        capture = text.index("uint32_t _t = s->r[25];")
        slot = text.index("s->r[25] = (s->r[9] + 0u);")
        call = text.index("dispatch(s, _t);")
        self.assertLess(capture, slot, "jr target captured after its delay slot")
        self.assertLess(slot, call, "delay slot emitted after the transfer")

    def test_jr_ra_still_returns_to_the_host_without_a_dispatch(self):
        """`jr $ra` IS the host return; it must not become a dispatch.

        This is the boundary the fix must not cross: the AOT frame model turns a
        `jr $ra` into a C return, and rewriting it as a computed dispatch would
        double-execute the caller's continuation.
        """
        text = self._emit({0x1000: JR_RA, 0x1004: NOP})
        self.assertNotIn("dispatch(s, _t);", text)
        self.assertIn("s->r[29] = _sp_entry; return;", text)

    def test_transfer_instruction_is_reported_before_its_delay_slot(self):
        """Trace order stays branch-then-slot, matching src/rt/guest_interp.c.

        The cosim comparator diffs the two lanes' canonical instruction traces
        line by line, so a reordering here would show up as a spurious divergence
        on every computed transfer rather than as a real defect.
        """
        text = self._emit(
            {
                0x1000: jalr(25, 8),
                0x1004: addu(8, 9, 0),
                0x1008: JR_RA,
                0x100C: NOP,
            }
        )
        transfer_report = text.index("sr_begin(s, 0x00001000u")
        slot_report = text.index("sr_begin(s, 0x00001004u")
        self.assertLess(transfer_report, slot_report)


if __name__ == "__main__":
    unittest.main()
