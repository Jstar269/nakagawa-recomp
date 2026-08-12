# SPDX-License-Identifier: GPL-2.0-or-later

import unittest

import codegen


class VfpuFallbackTests(unittest.TestCase):
    def test_unknown_vfpu_form_stays_inside_translated_function(self):
        word=(0x34<<26)|(31<<21)  # unsupported VFPU4 jump group
        line=codegen.normal_line(0x1234,word)
        self.assertIn("s->pc=0x00001234u",line)
        self.assertIn("sr_vfpu_interp(s,0xd3e00000u)",line)
        self.assertIn("sr_end",line)

    def test_left_right_quad_ops_always_use_single_step_decoder(self):
        for op in (0x35,0x3d):
            word=(op<<26)|(4<<21)|(7<<16)|2
            line=codegen.normal_line(0x2000,word)
            self.assertIn("s->pc=0x00002000u",line)
            self.assertIn("sr_vfpu_interp(s",line)

    def test_quad_register_bit_is_not_part_of_address_offset(self):
        # Bit zero extends vt from 5 to 6 bits.  It must be masked before sign-extending
        # the immediate, otherwise C code reads base+1 for registers 32..63.
        word=(0x36<<26)|(4<<21)|(1<<16)|1
        effect,_,_=codegen.vfpu_effect(0x3000,word)
        self.assertIn("s->r[4] + 0x00000000u",effect)
        self.assertNotIn("s->r[4] + 0x00000001u",effect)

    def test_vrot_overlap_recomputes_cosine_from_written_lane(self):
        # vt/op subfields select vrot; choose vd/vs in one matrix with scalar vs
        # present in the destination vector.
        word=(0x3c<<26)|(7<<23)|(29<<21)|(1<<16)|(0<<8)|0
        effect,_,_=codegen.vfpu_effect(0x4000,word)
        self.assertIn("sr_vfpu_cos(_d[0])",effect)
        self.assertRegex(effect,r"vfpuCtrl\[2\]&~0x[0-9a-f]+u")

    def test_vrot_overlap_ignores_inactive_lanes(self):
        # A scalar source register 0 must not match a zero-filled inactive
        # lane of a pair destination.
        word=(0x3c<<26)|(1<<7)|(7<<23)|(29<<21)|(1<<16)|(0<<8)|1
        effect,_,_=codegen.vfpu_effect(0x4100,word)
        self.assertNotRegex(effect,r"sr_vfpu_cos\(_d\[[23]\]\)")

    def test_vcrs_rejects_reserved_vector_widths(self):
        base=(0x19<<26)|(5<<23)|(1<<16)|(2<<8)|3
        for width_bits in (0, 1<<7, 1<<14 | 1<<7):
            with self.assertRaises(codegen.Unsupported):
                codegen.vfpu_effect(0x4200,base|width_bits)

    def test_allegrex_wsbw_is_translated(self):
        word=0x7C0A50E0
        effect,_,_=codegen.effect(0x32200254,word)
        self.assertIn("0xFF000000u",effect)
        self.assertIn("s->r[10] =",effect)


if __name__ == "__main__":
    unittest.main()
