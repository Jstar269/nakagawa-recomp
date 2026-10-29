# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen
import scratch_dir


CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


class VfpuFallbackTests(unittest.TestCase):
    def test_all_vfpu_control_registers_are_translated(self):
        for sub in (3, 7):  # mfvc / mtvc
            for imm in range(128, 144):
                word=(0x12<<26)|(sub<<21)|(5<<16)|imm
                with self.subTest(sub=sub, imm=imm):
                    effect, _, span = codegen.effect(0x1200 + imm, word)
                    self.assertEqual(span, 0)
                    self.assertNotIn("sr_vfpu_interp", effect)
                    self.assertIn(f"s->vfpuCtrl[{imm - 128}]", effect)

    def test_mfvc_into_zero_is_a_no_op_but_real_gpr_stays_unsupported(self):
        # `mfvc $zero, 255` (0x486000ff) is a compiler VFPU-sync idiom: writes to r0 are
        # discarded, so it must not reach the fail-closed interpreter fallback.
        effect, _, _ = codegen.vfpu_effect(0x08804000, 0x486000FF)
        self.assertNotIn("sr_vfpu_interp", effect)
        self.assertNotIn("s->r[", effect)
        with self.assertRaises(codegen.Unsupported):
            codegen.vfpu_effect(0x08804000, 0x486200FF)  # mfvc $v0, 255

    def test_unknown_vfpu_form_stays_inside_translated_function(self):
        word=(0x34<<26)|(31<<21)|0x8080  # vwbn.q: no measured rule (issue #326)
        line=codegen.normal_line(0x1234,word)
        self.assertIn("s->pc=0x00001234u",line)
        self.assertIn("sr_vfpu_interp(s,0xd3e08080u)",line)
        self.assertIn("SR_VFPU_OTHER",line)
        self.assertIn("VFPU_UNSUPPORTED",line)
        self.assertIn("sr_unimplemented",line)
        self.assertIn("issue=326",line)
        self.assertIn("sr_end",line)

    @unittest.skipUnless(CC, "a host C compiler is required")
    def test_unknown_vfpu_form_stops_before_following_instruction(self):
        word = (0x34 << 26) | (31 << 21) | 0x8080  # vwbn.q: no measured rule
        line = codegen.normal_line(0x1234, word)
        source = f'''#include <stdint.h>
#include <stdio.h>
#define SR_VFPU_OTHER 0
typedef struct CpuState {{ uint32_t pc; uint32_t r[32]; }} CpuState;
static void sr_begin(CpuState *s, uint32_t pc, uint32_t op) {{ (void)s; (void)pc; (void)op; }}
static void sr_end(CpuState *s, uint32_t addr, uint32_t size) {{ (void)s; (void)addr; (void)size; }}
static int sr_vfpu_interp(CpuState *s, uint32_t op) {{ (void)s; return op == 0xd3e08080u ? SR_VFPU_OTHER : 1; }}
static int unsupported;
static void sr_unimplemented(uint32_t pc, const char *reason) {{ (void)pc; (void)reason; unsupported = 1; }}
static void generated(CpuState *s) {{ {line} s->r[0] = 0xdeadbeefu; }}
int main(void) {{ CpuState s = {{0}}; generated(&s); return unsupported == 1 && s.r[0] == 0u ? 0 : 1; }}
'''
        with scratch_dir.TemporaryDirectory(prefix="vfpu_census_fallback_") as tmp:
            root = Path(tmp)
            source_path = root / "fallback.c"
            exe_path = root / ("fallback.exe" if Path(CC).suffix.lower() == ".exe" else "fallback")
            source_path.write_text(source, encoding="ascii", newline="\n")
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", str(source_path), "-o", str(exe_path)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)

    def test_measured_vfpu7_forms_route_to_the_interpreter(self):
        # vrnds.s, vrndi/vrndf1/vrndf2 at .p/.t/.q, vf2h.p/.q and vc2i.s follow the
        # 2026-10-29 PSP-3000 measurement PSP-VFPU-FORMS2-001 (issue #326), implemented
        # once in sr_vfpu_interp. The AOT routes exactly those widths and raises
        # Unsupported for the rest, which still fail closed through the interpreter.
        routed = (0xD0200000, 0xD0210080, 0xD0218000, 0xD0218080, 0xD0220080,
                  0xD0238080, 0xD0320088, 0xD0328088, 0xD0390008)
        for word in routed:
            with self.subTest(word=f"0x{word:08x}"):
                body, saddr, ssize = codegen.vfpu_effect(0x3400, word)
                self.assertEqual((saddr, ssize), (None, 0))
                self.assertIn("s->pc=0x00003400u", body)
                self.assertIn(f"sr_vfpu_interp(s,0x{word:08x}u)", body)
                self.assertIn("VFPU_UNSUPPORTED", body)
        for word in (0xD0200080, 0xD0320008, 0xD0328008, 0xD0390088):
            with self.subTest(word=f"0x{word:08x}"):
                with self.assertRaises(codegen.Unsupported):
                    codegen.vfpu_effect(0x3400, word)
                line = codegen.normal_line(0x3400, word)
                self.assertIn(f"sr_vfpu_interp(s,0x{word:08x}u)", line)
                self.assertIn("SR_VFPU_OTHER", line)
                self.assertIn("issue=326", line)
                self.assertIn("return;", line)

    def test_vrndi_single_steps_the_random_unit_directly(self):
        # vrndi.s follows the PSP-HLE-ISSUES-001 step rule (issue #326): the AOT emits it
        # without the interpreter, through the shared sr_vfpu_rnd_step kernel.
        for word, lane in ((0xD0210000, 0), (0xD021006E, 59)):
            with self.subTest(word=f"0x{word:08x}"):
                body, saddr, ssize = codegen.vfpu_effect(0x3400, word)
                self.assertEqual((saddr, ssize), (None, 0))
                self.assertNotIn("sr_vfpu_interp", body)
                self.assertIn(f"s->vi[{lane}]=sr_vfpu_rnd_step(&s->vfpuCtrl[8]);", body)
                for token in ("VFPU_UNSUPPORTED", "issue=326", "sr_unimplemented", "return;"):
                    self.assertIn(token, body)

    @unittest.skipUnless(CC, "a host C compiler is required")
    def test_vrndi_single_under_a_prefix_stops_before_following_instruction(self):
        # A pending S, T or D prefix on vrndi.s is unmeasured: the generated body must stop
        # the run with the state untouched, as sr_vfpu_interp refuses it. With identity
        # prefixes it draws once into the destination lane and the next instruction runs.
        line = codegen.normal_line(0x1234, 0xD021006E)
        source = f'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct CpuState {{ uint32_t pc; uint32_t r[32]; uint32_t vi[128]; uint32_t vfpuCtrl[16]; }} CpuState;
static void sr_begin(CpuState *s, uint32_t pc, uint32_t op) {{ (void)s; (void)pc; (void)op; }}
static void sr_end(CpuState *s, uint32_t addr, uint32_t size) {{ (void)s; (void)addr; (void)size; }}
static uint32_t sr_vfpu_rnd_step(uint32_t *rcx) {{ rcx[0] += 1u; return 0x5eed1234u; }}
static int unsupported;
static void sr_unimplemented(uint32_t pc, const char *reason) {{ (void)pc; (void)reason; unsupported = 1; }}
static void generated(CpuState *s) {{ {line} s->r[1] = 0xdeadbeefu; }}
static int run(uint32_t sp, uint32_t tp, uint32_t dp, int expect_draw) {{
    CpuState s, before;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = sp; s.vfpuCtrl[1] = tp; s.vfpuCtrl[2] = dp; s.vfpuCtrl[8] = 7u;
    before = s;
    unsupported = 0;
    generated(&s);
    if (expect_draw)
        return !unsupported && s.vi[59] == 0x5eed1234u && s.vfpuCtrl[8] == 8u &&
               s.r[1] == 0xdeadbeefu && s.pc == 0u && memcmp(s.vfpuCtrl, before.vfpuCtrl, 8 * sizeof(uint32_t)) == 0;
    return unsupported && s.r[1] == 0u && s.pc == 0x1234u && s.vi[59] == 0u &&
           memcmp(s.vfpuCtrl, before.vfpuCtrl, sizeof(s.vfpuCtrl)) == 0;
}}
int main(void) {{
    int ok = run(0xe4u, 0xe4u, 0u, 1);
    ok &= run(0xf00e4u, 0xe4u, 0u, 0);
    ok &= run(0xe4u, 0x1bu, 0u, 0);
    ok &= run(0xe4u, 0xe4u, 0x100u, 0);
    ok &= run(0xe4u, 0xe4u, 0x1u, 0);
    return ok ? 0 : 1;
}}
'''
        with scratch_dir.TemporaryDirectory(prefix="vfpu_vrndi_prefix_") as tmp:
            root = Path(tmp)
            source_path = root / "vrndi.c"
            exe_path = root / ("vrndi.exe" if Path(CC).suffix.lower() == ".exe" else "vrndi")
            source_path.write_text(source, encoding="ascii", newline="\n")
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", str(source_path), "-o", str(exe_path)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)

    def test_vfpu_forms_are_direct_at_their_measured_widths(self):
        # PSP-VFPU-FORMS-001 (issue #326): vh2f.s/.p, vuc2i.s, vsocp.s/.p, vwbn.s and
        # vrndf1.s/vrndf2.s are emitted without the interpreter, through the recomp.h
        # helpers sr_vfpu_interp also calls. Every other width raises Unsupported.
        direct = {
            0xD0330080: ("s->vi[3]=sr_vfpu_h2f(_s1>>16);",),           # FF III's vh2f.p
            0xD0330008: ("s->vi[33]=sr_vfpu_h2f(_s0>>16);",),
            0xD0380100: ("uint32_t _s=s->vi[4];", "s->vi[3]=sr_vfpu_uc2i(_s>>24);"),
            0xD0456041: ("_x[0]=s->vi[3];", "s->vi[7]=_d[1];"),          # SSP's vsocp.s
            0xD0450088: ("sr_vfpu_socp(_x[1],&_d[2]);", "s->vi[35]=_d[3];"),
            0xD37F0007: ("s->vi[28]=sr_vfpu_wbn(s->vi[0],127u);",),      # SSP's vwbn.s
            0xD0220000: ("s->vi[0]=sr_vfpu_rndf_step(&s->vfpuCtrl[8],0);",),
            0xD0230000: ("s->vi[0]=sr_vfpu_rndf_step(&s->vfpuCtrl[8],1);",),
        }
        for word, fragments in direct.items():
            with self.subTest(word=f"0x{word:08x}"):
                body, saddr, ssize = codegen.vfpu_effect(0x3400, word)
                self.assertEqual((saddr, ssize), (None, 0))
                self.assertNotIn("sr_vfpu_interp", body)
                for fragment in fragments:
                    self.assertIn(fragment, body)
                for token in ("VFPU_UNSUPPORTED", "issue=326", "sr_unimplemented", "return;"):
                    self.assertIn(token, body)
        # The in-place vh2f.p reads both source words before the first write.
        body = codegen.vfpu_effect(0x3400, 0xD0330080)[0]
        self.assertLess(body.index("uint32_t _s1=s->vi[1];"), body.index("s->vi[0]=sr_vfpu_h2f"))
        for word in (0xD0338008, 0xD0338088, 0xD0380088, 0xD0458008, 0xD0458088,
                     0xD37F0088, 0xD3E08080):
            with self.subTest(word=f"0x{word:08x}"):
                with self.assertRaises(codegen.Unsupported):
                    codegen.vfpu_effect(0x3400, word)
                line = codegen.normal_line(0x3400, word)
                self.assertIn(f"sr_vfpu_interp(s,0x{word:08x}u)", line)
                self.assertIn("issue=326", line)

    @unittest.skipUnless(CC, "a host C compiler is required")
    def test_vfpu_forms_under_a_prefix_stop_before_following_instruction(self):
        # A pending S, T or D prefix on these forms is unmeasured: the generated body must
        # stop the run with the state untouched, as sr_vfpu_interp refuses it. With
        # identity prefixes the destination lanes change and the next instruction runs.
        line = codegen.normal_line(0x1234, 0xD0330080)  # vh2f.p C000, C000
        source = f'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct CpuState {{ uint32_t pc; uint32_t r[32]; uint32_t vi[128]; uint32_t vfpuCtrl[16]; }} CpuState;
static void sr_begin(CpuState *s, uint32_t pc, uint32_t op) {{ (void)s; (void)pc; (void)op; }}
static void sr_end(CpuState *s, uint32_t addr, uint32_t size) {{ (void)s; (void)addr; (void)size; }}
static uint32_t sr_vfpu_h2f(uint32_t h) {{ return 0x10000u | h; }}
static int unsupported;
static void sr_unimplemented(uint32_t pc, const char *reason) {{ (void)pc; (void)reason; unsupported = 1; }}
static void generated(CpuState *s) {{ {line} s->r[1] = 0xdeadbeefu; }}
static int run(uint32_t sp, uint32_t tp, uint32_t dp, int expect_write) {{
    CpuState s, before;
    memset(&s, 0, sizeof(s));
    s.vi[0] = 0x22221111u; s.vi[1] = 0x44443333u;
    s.vfpuCtrl[0] = sp; s.vfpuCtrl[1] = tp; s.vfpuCtrl[2] = dp;
    before = s;
    unsupported = 0;
    generated(&s);
    if (expect_write)
        return !unsupported && s.vi[0] == 0x11111u && s.vi[1] == 0x12222u &&
               s.vi[2] == 0x13333u && s.vi[3] == 0x14444u && s.r[1] == 0xdeadbeefu &&
               s.pc == 0u && memcmp(s.vfpuCtrl, before.vfpuCtrl, sizeof(s.vfpuCtrl)) == 0;
    return unsupported && s.r[1] == 0u && s.pc == 0x1234u &&
           memcmp(s.vi, before.vi, sizeof(s.vi)) == 0 &&
           memcmp(s.vfpuCtrl, before.vfpuCtrl, sizeof(s.vfpuCtrl)) == 0;
}}
int main(void) {{
    int ok = run(0xe4u, 0xe4u, 0u, 1);
    ok &= run(0xf00e4u, 0xe4u, 0u, 0);
    ok &= run(0xe4u, 0x1bu, 0u, 0);
    ok &= run(0xe4u, 0xe4u, 0x100u, 0);
    ok &= run(0xe4u, 0xe4u, 0x1u, 0);
    return ok ? 0 : 1;
}}
'''
        with scratch_dir.TemporaryDirectory(prefix="vfpu_forms_prefix_") as tmp:
            root = Path(tmp)
            source_path = root / "forms.c"
            exe_path = root / ("forms.exe" if Path(CC).suffix.lower() == ".exe" else "forms")
            source_path.write_text(source, encoding="ascii", newline="\n")
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", str(source_path), "-o", str(exe_path)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)

    def test_measured_vfpu9_forms_route_to_the_interpreter(self):
        # vsgn, vbfy1, vbfy2 and vt4444 follow the 2026-10-24 PSP-3000 measurement
        # (issue #326), implemented once in sr_vfpu_interp. The AOT routes exactly
        # the widths the interpreter accepts and raises Unsupported for the rest.
        routed = (0xD04A0405, 0xD04A0020, 0xD04A8080, 0xD0420183, 0xD0428080,
                  0xD0438183, 0xD0599090)
        for word in routed:
            with self.subTest(word=f"0x{word:08x}"):
                body, saddr, ssize = codegen.vfpu_effect(0x3400, word)
                self.assertEqual((saddr, ssize), (None, 0))
                self.assertIn("s->pc=0x00003400u", body)
                self.assertIn(f"sr_vfpu_interp(s,0x{word:08x}u)", body)
                self.assertIn("VFPU_UNSUPPORTED", body)
        for word in (0xD0420000, 0xD0428000, 0xD0430080, 0xD0590080, 0xD0598000):
            with self.subTest(word=f"0x{word:08x}"):
                with self.assertRaises(codegen.Unsupported):
                    codegen.vfpu_effect(0x3400, word)
                line = codegen.normal_line(0x3400, word)
                self.assertIn("issue=326", line)
                self.assertIn("return;", line)

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

    def test_vrexp2_is_exp2_of_the_negated_operand_at_every_width(self):
        # PSP-VFPU-TRANSCENDENTAL-001: a PSP-3000 returned exp2(-x) for vrexp2.s (1.0 for
        # zero, 2.0 for -1.0). Optype 28 of the VV2 group is that instruction, not a negated
        # exp2; vexp2 (optype 20) stays the plain exp2.
        for size in (1, 2, 3, 4):
            lo, hi = (size - 1) & 1, (size - 1) >> 1
            for optype, want in ((28, "sr_vfpu_exp2(-_s[_i])"), (20, "sr_vfpu_exp2(_s[_i])")):
                word = ((0x34 << 26) | (optype << 16) | (0 << 8) | 0x40 | (lo << 7) | (hi << 15))
                with self.subTest(size=size, optype=optype):
                    effect, _, _ = codegen.vfpu_effect(0x4200, word)
                    self.assertIn(want, effect)
                    self.assertNotIn("-sr_vfpu_exp2(", effect)

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

    def test_vmmul_accumulates_into_a_local_like_the_interpreter(self):
        # Issue #40: a single chained expression (0.0f+t0+t1+...) compiles to a
        # different addss operand order at -O0 than sr_vfpu_interp's loop, and
        # x86-64 SSE selects the *first* NaN source on NaN+NaN, so the two paths
        # kept different NaN payloads for exotic matrix inputs. The emission must
        # stay an accumulate-into-local so the paths agree on any host.
        word=(0x3c<<26)|(0<<23)|(4<<16)|(2<<8)|(1<<15)  # vmmul 3x3, vs=1, vt=0, vd=0
        effect,_,_=codegen.vfpu_effect(0x5000,word)
        self.assertIn("float _m0_0=0.0f;",effect)
        self.assertIn("_m0_0+=s->v[",effect)
        self.assertNotRegex(effect, r"float _m[0-9]_[0-9]=0\.0f\+",
                            "chained vmmul expression reintroduces the NaN payload split")

    def test_vtfm_accumulates_into_a_local_like_the_interpreter(self):
        word=(0x3c<<26)|(1<<23)|(1<<16)|(2<<8)|0  # vtfm3 3-lane, ins=1
        effect,_,_=codegen.vfpu_effect(0x5100,word)
        self.assertIn("float _v0=0.0f;",effect)
        self.assertIn("_v0+=s->v[",effect)
        self.assertNotRegex(effect, r"float _v[0-9]=0\.0f\+",
                            "chained vtfm expression reintroduces the NaN payload split")

    def test_vhdp_accumulates_into_a_local_with_ordered_loop(self):
        # Issue #40: vhdp must accumulate with an ordered loop so the host
        # compiler cannot reassociate the sum and pick a different NaN payload.
        word=(0x19<<26)|(4<<23)|(0<<16)|(1<<15)|(0x20<<8)|(0x20)  # vhdp.t 3-lane, vd=0x20, vs=0x20, vt=0x00
        effect,_,_=codegen.vfpu_effect(0x5200,word)
        self.assertIn("float _d=0.0f;",effect)
        self.assertIn("for(int _i=0;_i<2;_i++) _d+=_s[_i]*_t[_i];",effect)
        self.assertIn("_d+=1.0f*_t[2];",effect)
        self.assertNotRegex(effect, r"float _d=0\.0f\+",
                            "chained vhdp expression reintroduces the NaN payload split")

    def test_vmscl_snapshots_scalar_before_destructive_writes(self):
        # Issue #40: vmscl must snapshot the scalar before any destination write
        # so overlapping scalar-in-destination reads the original value.
        word=(0x3c<<26)|(4<<23)|(0<<16)|(1<<15)|(0x20<<8)|(0x20)  # vmscl.t 3x3, vd=0x20, vs=0x20, vt=0x00
        effect,_,_=codegen.vfpu_effect(0x5300,word)
        self.assertIn("float _sc = s->v[0];",effect)
        self.assertIn("s->v[0] = s->v[0] * _sc;",effect)
        self.assertNotIn("s->v[0] * s->v[0]",effect,
                         "vmscl must not read the scalar fresh after it is overwritten")

    def test_vmscl_alias_snapshots_scalar_before_destructive_writes(self):
        # Issue #40: Matrix1 vmscl alias path has the same scalar-overlap contract.
        word=(0x3c<<26)|(7<<23)|(1<<16)|(1<<15)|(0x20<<8)|(0x20)  # Matrix1 which=1, vd=0x20, vs=0x20
        effect,_,_=codegen.vfpu_effect(0x5400,word)
        self.assertIn("float _sc = s->v[4];",effect)
        self.assertIn("s->v[0] = s->v[0] * _sc;",effect)
        self.assertNotIn("s->v[0] * s->v[4]",effect,
                         "vmscl alias must not read the scalar fresh after it is overwritten")


if __name__ == "__main__":
    unittest.main()
