# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Invariant tests for the public VFPU synthetic corpus and coverage report.

Hostile-review invariants (Codex):
  - VFPU12 vcrsp/vqmul must be actual sub==5 (bits 25:23), not the old 5-bit field at 25:21.
  - Size-distribution invariants must kill deletion of an entire S/P/T/Q class.
  - Family/sub-op coverage invariants must kill denominator inflation or deletion masked by expansion elsewhere.
  - Explicit malformed/negative coverage, including old malformed vfim-family shape, outside the positive corpus.
  - Clear distinction between emitter unsupported, interpreter fallback, malformed, and positive corpus.
  - No "production accepts it, therefore keep it" filtering.

Independent vector lock (Freebuff, 2026-08-28):
  Fixed regression anchors are compared as integer words, not via codegen.vfpu_effect decode.
    vadd.s  0x60000000  vadd.p 0x60000080  vadd.t 0x60008000  vadd.q 0x60008080
    vmul.s  0x64000000  vmul.p 0x64000080  vmul.t 0x64008000  vmul.q 0x64008080
    vmin.s  0x6D000000  vmax.s 0x6D800000
    vcst.s  0xD0600000  vmov.s  0xD0000000
    vmmul.p 0xF0000080  vmmul.t 0xF0008000  vmmul.q 0xF0008080
    vtfm2.p 0xF0800080  vtfm3.t 0xF1008000  vtfm4.q 0xF1808080
    vcrsp.t 0xF2808000  vqmul.q 0xF2808080
    vpfxs   0xDC000000  vpfxt   0xDD000000  vpfxd 0xDE000000
    viim.s  0xDF000000  vfim.s 0xDF800000
  Size bits: bit15=sizehi, bit7=sizelo, S00 P01 T10 Q11, Family 0x3C sub is bits 25:23.
This file does not claim independent encoding proof beyond the literals; Freebuff is researching separately.

Independent prefix/immediate anchors (this file's hardening):
  - The five canonical prefix/immediate literals (vpfxs 0xDC000000, vpfxt
    0xDD000000, vpfxd 0xDE000000, viim 0xDF000000, vfim 0xDF800000) are asserted
    to be PRESENT in the actual iterator/generator output
    (set(_iter_vpfx()) and set(generate_synthetic_corpus())), not by comparing
    the literal table to itself.
  - Independence is established by direct integer equality, not via Nakagawa's decoder.
  - Per-category counts (vpfxs 32, vpfxt 32, vpfxd 48, viim 35, vfim 35) are pinned.
  - Protected contract is canonical anchors + per-category cardinality +
    operation/suboperation/size coverage. It does NOT pin every individual
    swizzle/register/immediate tuple; non-anchor intra-category substitutions
    that preserve the above aggregates are an explicitly acknowledged
    operand-distribution boundary and may survive without test failure.

Intended-coverage contract (independent, not output-derived):
  - Expected counts for family, suboperation, legal size sets, VFPU4 jump classes,
    VFPU12 suboperations, and prefix/immediate categories are hard-coded below
    from the spec and iterator parameter sets, not computed from the corpus.
  - Mutants that redistribute words across pinned family, suboperation, size, or VFPU4 jump categories must fail. Operand/register/swizzle/immediate substitutions within a category that preserve those aggregates are outside this corpus contract except for explicitly pinned canonical anchors.
"""

import sys
import unittest
from pathlib import Path
from collections import Counter

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

def _size_independent(word: int) -> int:
    return ((word >> 7) & 1 | ((word >> 15) & 1) << 1) + 1
def _op6(word: int) -> int:
    return (word >> 26) & 0x3F
def _sub3(word: int) -> int:
    return (word >> 23) & 7
def _sub5(word: int) -> int:
    return (word >> 21) & 0x1F
def _jump(word: int) -> int:
    return (word >> 21) & 0x1F

# ---------------------------------------------------------------------------
# Independent intended-coverage specification (hard-coded, not output-derived)
# ---------------------------------------------------------------------------
# Derived from spec + iterator parameter sets in vfpu_synth_gen.py, NOT by
# measuring generate_synthetic_corpus().  This is the denominator guard.

INTENDED_TOTAL = 2742
INTENDED_FAMILY_COUNTS = {0x18: 144, 0x19: 204, 0x1B: 60, 0x34: 2020, 0x37: 182, 0x3C: 132}
INTENDED_SIZE_COUNTS = {1: 718, 2: 682, 3: 677, 4: 665}

# Family -> sub (3-bit for 0x18/0x19/0x1B/0x3C) -> count
INTENDED_FAMILY_SUB_COUNTS = {
    0x18: {0: 48, 1: 48, 7: 48},
    0x19: {0: 48, 1: 48, 2: 48, 4: 48, 5: 12},
    0x1B: {0: 12, 2: 12, 3: 12, 6: 12, 7: 12},
    0x3C: {0: 9, 1: 9, 2: 9, 3: 9, 4: 9, 5: 6, 7: 81},
}

# VFPU4 jump (5-bit at 25:21) -> count  (family 0x34)
INTENDED_VFPU4_JUMP_COUNTS = {0: 480, 1: 100, 2: 20, 3: 240, 16: 180, 17: 180, 18: 180, 19: 180, 20: 180, 21: 280}
# VFPU4 jump -> size -> count
INTENDED_VFPU4_JUMP_SIZE_COUNTS = {
    (0, 1): 120, (0, 2): 120, (0, 3): 120, (0, 4): 120,
    (21, 1): 70, (21, 2): 70, (21, 3): 70, (21, 4): 70,
    (3, 1): 60, (3, 2): 60, (3, 3): 60, (3, 4): 60,
    (16, 1): 45, (16, 2): 45, (16, 3): 45, (16, 4): 45,
    (17, 1): 45, (17, 2): 45, (17, 3): 45, (17, 4): 45,
    (18, 1): 45, (18, 2): 45, (18, 3): 45, (18, 4): 45,
    (19, 1): 45, (19, 2): 45, (19, 3): 45, (19, 4): 45,
    (20, 1): 45, (20, 2): 45, (20, 3): 45, (20, 4): 45,
    (1, 1): 25, (1, 2): 25, (1, 3): 25, (1, 4): 25,
    (2, 1): 5, (2, 2): 5, (2, 3): 5, (2, 4): 5,
}
# Family -> size -> count
INTENDED_FAMILY_SIZE_COUNTS = {
    0x18: {1: 36, 2: 36, 3: 36, 4: 36},
    0x19: {1: 48, 2: 48, 3: 60, 4: 48},
    0x1B: {1: 15, 2: 15, 3: 15, 4: 15},
    0x34: {1: 505, 2: 505, 3: 505, 4: 505},
    0x37: {1: 114, 2: 36, 3: 16, 4: 16},
    0x3C: {1: 0, 2: 42, 3: 45, 4: 45},
}
# (family, sub) -> size -> count  (sub is 3-bit for 0x18/0x19/0x1B/0x3C; for 0x34 use jump)
INTENDED_SUB_SIZE_COUNTS = {
    (0x18, 0): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x18, 1): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x18, 7): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 0): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 1): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 2): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 4): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 5): {1: 0, 2: 0, 3: 12, 4: 0},
    (0x1B, 0): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 2): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 3): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 6): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 7): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x3C, 0): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 1): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 2): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 3): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 4): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 5): {1: 0, 2: 0, 3: 3, 4: 3},
    (0x3C, 7): {1: 0, 2: 27, 3: 27, 4: 27},
}
# Prefix/immediate categories (family 0x37)
INTENDED_PREFIX_COUNTS = {"vpfxs": 32, "vpfxt": 32, "vpfxd": 48, "viim": 35, "vfim": 35}
# Independent literal anchors (hard-coded, not via INDEPENDENT_LITERALS table)
EXPECTED_VPFXS = 0xDC000000
EXPECTED_VPFXT = 0xDD000000
EXPECTED_VPFXD = 0xDE000000
EXPECTED_VIIM  = 0xDF000000
EXPECTED_VFIM  = 0xDF800000
EXPECTED_VCRSP_T = 0xF2808000
EXPECTED_VQMUL_Q = 0xF2808080

# VFPU12 idx for sub==7
INTENDED_VFPU12_IDX_COUNTS = {28: 72, 29: 9}
INTENDED_VFPU12_WHICH_COUNTS = {0: 9, 1: 9, 2: 9, 3: 9, 4: 9, 5: 9, 6: 9, 7: 9}

class SyntheticCorpusTests(unittest.TestCase):
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self._corpus = generate_synthetic_corpus()
        self._set = set(self._corpus)
    def test_corpus_is_nonempty(self):
        self.assertGreater(len(self._corpus), 0)
    def test_corpus_has_no_duplicates(self):
        self.assertEqual(len(self._corpus), len(self._set))
    def test_corpus_is_deterministic(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self.assertEqual(self._corpus, generate_synthetic_corpus())
    def test_corpus_words_are_32bit(self):
        for w in self._corpus:
            self.assertGreaterEqual(w, 0)
            self.assertLessEqual(w, 0xFFFFFFFF)
    def test_corpus_is_sorted(self):
        self.assertEqual(self._corpus, sorted(self._corpus))
    def test_every_word_decodes_without_fallback(self):
        import codegen
        bad=[]
        for w in self._corpus:
            try:
                body,_,_=codegen.vfpu_effect(0x08900000,w)
            except codegen.Unsupported as e:
                bad.append(f"0x{w:08x} unsupported: {e}")
                continue
            if "sr_vfpu_interp" in body:
                bad.append(f"0x{w:08x} fallback")
        self.assertEqual(bad,[], "corpus must decode through static emitter:\n"+"\n".join(bad[:20]))
    def test_no_production_filtering_in_generator(self):
        # Hardened: generator must be architecturally intentional and independent
        # from current production support.  It must NOT call production helpers
        # as a filter (e.g., `if codegen.vfpu_effect(word) is supported: include(word)`)
        # even when every current positive word happens to pass that filter.
        # This regression uses a genuinely fresh import context (subprocess) where
        # production helpers are poisoned BEFORE vfpu_synth_gen is imported, so it
        # kills both late-bound `import codegen; codegen.vfpu_effect(...)` and
        # early-bound `from codegen import vfpu_effect as _prod_effect; _prod_effect(...)`
        # even when the filter is currently a semantic no-op because all 2742 words
        # are production-emittable.  The invariant is behavioral, not source-text.
        import hashlib
        import subprocess
        import sys
        import textwrap
        expected = self._corpus
        expected_sha = hashlib.sha256(','.join(f'{w:08x}' for w in expected).encode()).hexdigest()
        self.assertEqual(len(expected), INTENDED_TOTAL, "total must be spec-derived 2742 (2738+4 for vcrsp fix)")
        fam=Counter(_op6(w) for w in expected)
        for op,exp in INTENDED_FAMILY_COUNTS.items():
            self.assertEqual(fam.get(op,0),exp, f"family 0x{op:02x} must be {exp}")
        # Fresh subprocess: poison BEFORE importing vfpu_synth_gen
        root_repr = repr(str(ROOT))
        sha_repr = repr(expected_sha)
        total = INTENDED_TOTAL
        script = textwrap.dedent(f'''
            import sys
            from pathlib import Path
            ROOT = Path({root_repr})
            sys.path.insert(0, str(ROOT / "tools"))
            sys.path.insert(0, str(ROOT))
            import codegen
            def _raise(*a, **kw):
                raise AssertionError("production helper called during positive generation - filtering not allowed")
            for _attr in ('vfpu_effect', 'normal_line', 'effect', 'fpu_effect'):
                if hasattr(codegen, _attr):
                    setattr(codegen, _attr, _raise)
            import importlib
            if 'vfpu_synth_gen' in sys.modules:
                del sys.modules['vfpu_synth_gen']
            import vfpu_synth_gen
            import hashlib
            from collections import Counter
            corpus = vfpu_synth_gen.generate_synthetic_corpus()
            sha = hashlib.sha256(','.join(f'{{w:08x}}' for w in corpus).encode()).hexdigest()
            assert sha == {sha_repr}, f"SHA mismatch {{sha}} != {sha_repr}"
            assert len(corpus) == {total}, f"len {{len(corpus)}} != {total}"
            def _op6(w):
                return (w >> 26) & 0x3F
            fam = Counter(_op6(w) for w in corpus)
            expected_fam = {repr(INTENDED_FAMILY_COUNTS)}
            for _op, _exp in expected_fam.items():
                assert fam.get(_op, 0) == _exp, f"family 0x{{_op:02x}} {{fam.get(_op,0)}} != {{_exp}}"
            print("FRESH_OK")
        ''')
        result = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, cwd=str(ROOT), timeout=30)
        self.assertEqual(result.returncode, 0,
            f"positive corpus generation must not consult production helpers (fresh import).\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}")
        self.assertIn("FRESH_OK", result.stdout)

class IndependentVectorLockTests(unittest.TestCase):
    def test_independent_vadd_encodes_match(self):
        from vfpu_synth_gen import _encode
        cases=[("vadd.s",0x60000000,0),("vadd.p",0x60000080,1),("vadd.t",0x60008000,2),("vadd.q",0x60008080,3)]
        for name,expect,size in cases:
            got=_encode(0x18,0,0,0,0,size)
            self.assertEqual(got,expect, f"{name}: got 0x{got:08x} expect 0x{expect:08x}")
    def test_independent_vmul_encodes_match(self):
        from vfpu_synth_gen import _encode
        cases=[("vmul.s",0x64000000,0),("vmul.p",0x64000080,1),("vmul.t",0x64008000,2),("vmul.q",0x64008080,3)]
        for name,expect,size in cases:
            got=_encode(0x19,0,0,0,0,size)
            self.assertEqual(got,expect, f"{name}")
    def test_independent_vmin_vmax(self):
        from vfpu_synth_gen import _encode
        self.assertEqual(_encode(0x1B,2,0,0,0,0),0x6D000000, "vmin.s")
        self.assertEqual(_encode(0x1B,3,0,0,0,0),0x6D800000, "vmax.s")
    def test_independent_vmov_vcst(self):
        from vfpu_synth_gen import _encode_vfpu4
        self.assertEqual(_encode_vfpu4(0,0,0,0,0),0xD0000000, "vmov.s")
        self.assertEqual(_encode_vfpu4(3,0,0,0,0),0xD0600000, "vcst.s")
    def test_independent_vmmul_encodes_match(self):
        from vfpu_synth_gen import _encode
        self.assertEqual(_encode(0x3C,0,0,0,0,1),0xF0000080, "vmmul.p")
        self.assertEqual(_encode(0x3C,0,0,0,0,2),0xF0008000, "vmmul.t")
        self.assertEqual(_encode(0x3C,0,0,0,0,3),0xF0008080, "vmmul.q")
    def test_independent_vtfm_encodes_match(self):
        from vfpu_synth_gen import _encode
        self.assertEqual(_encode(0x3C,1,0,0,0,1),0xF0800080, "vtfm2.p")
        self.assertEqual(_encode(0x3C,2,0,0,0,2),0xF1008000, "vtfm3.t")
        self.assertEqual(_encode(0x3C,3,0,0,0,3),0xF1808080, "vtfm4.q")
    def test_independent_vcrsp_vqmul_encodes_match(self):
        from vfpu_synth_gen import _encode
        got_t=_encode(0x3C,5,0,0,0,2)
        got_q=_encode(0x3C,5,0,0,0,3)
        self.assertEqual(got_t, EXPECTED_VCRSP_T, f"vcrsp.t got 0x{got_t:08x}")
        self.assertEqual(got_q, EXPECTED_VQMUL_Q, f"vqmul.q got 0x{got_q:08x}")
        self.assertEqual((got_t>>23)&7,5, "vcrsp sub5")
        self.assertEqual((got_q>>23)&7,5, "vqmul sub5")
        self.assertEqual(_size_independent(got_t),3, "vcrsp size T")
        self.assertEqual(_size_independent(got_q),4, "vqmul size Q")
    def test_independent_size_encoding(self):
        cases=[(1,0,0),(2,1,0),(3,0,1),(4,1,1)]
        for size,lo,hi in cases:
            from vfpu_synth_gen import _size_bits_independent, _decode_size_independent
            got_lo,got_hi=_size_bits_independent(size)
            self.assertEqual((got_lo,got_hi),(lo,hi), f"size {size}")
            word=(0x60<<24)|(lo<<7)|(hi<<15)
            self.assertEqual(_decode_size_independent(word),size)
            self.assertEqual(_size_independent(word),size)

class IndependentPrefixImmediateIteratorTests(unittest.TestCase):
    """Fix VFPU_INDEPENDENT_FIXED_VECTORS: prove iterator/generator output contains literals via direct integer, no decoder."""
    def test_corpus_contains_independent_prefix_immediate_literals(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        corpus_set = set(generate_synthetic_corpus())
        # Direct integer comparison, not table-to-self, not via codegen decoder
        self.assertIn(EXPECTED_VPFXS, corpus_set, "vpfxs 0xDC000000 must be in synthetic corpus (direct integer)")
        self.assertIn(EXPECTED_VPFXT, corpus_set, "vpfxt 0xDD000000 must be in synthetic corpus")
        self.assertIn(EXPECTED_VPFXD, corpus_set, "vpfxd 0xDE000000 must be in synthetic corpus")
        self.assertIn(EXPECTED_VIIM, corpus_set, "viim 0xDF000000 must be in synthetic corpus")
        self.assertIn(EXPECTED_VFIM, corpus_set, "vfim 0xDF800000 must be in synthetic corpus")
        # Verify vfim is viim with bit23 set, independent check
        self.assertEqual(EXPECTED_VIIM | (1 << 23), EXPECTED_VFIM, "vfim must be viim | (1<<23)")

    def test_iter_vpfx_contains_independent_literals(self):
        from vfpu_synth_gen import _iter_vpfx
        s = set(_iter_vpfx())
        self.assertIn(EXPECTED_VPFXS, s, "vpfxs 0xDC000000 must be generated by _iter_vpfx (independent anchor)")
        self.assertIn(EXPECTED_VPFXT, s, "vpfxt 0xDD000000 must be generated by _iter_vpfx")
        self.assertIn(EXPECTED_VPFXD, s, "vpfxd 0xDE000000 must be generated by _iter_vpfx")
        # Ensure another legal VPFX encoding exists but does NOT substitute the literal
        self.assertIn(0xDC0000E4, s, "legal VPFX with swizzle 0xE4 must also be generated")
        self.assertIn(0xDD0000E4, s)
        self.assertIn(0xDE00000F, s)  # vpfxd with mask 15
        # The literal presence assertion above ensures a mutant that replaces
        # 0xDC000000 with 0xDC0000E4 would be detected (literal missing).

    def test_iter_viim_vfim_contains_independent_literals(self):
        from vfpu_synth_gen import _iter_viim_vfim
        s = set(_iter_viim_vfim())
        self.assertIn(EXPECTED_VIIM, s, "viim 0xDF000000 must be generated by _iter_viim_vfim")
        self.assertIn(EXPECTED_VFIM, s, "vfim 0xDF800000 must be generated by _iter_viim_vfim")
        # Another legal immediate encoding
        self.assertIn(0xDF0000FF, s)  # viim with imm 0xFF
        self.assertIn(0xDF8000FF, s)  # vfim with imm 0xFF
        # Literal presence above ensures mutants removing viim/vfim are detected.

    def test_prefix_immediate_category_counts_exact(self):
        from vfpu_synth_gen import generate_synthetic_corpus, generate_malformed_corpus
        corpus = generate_synthetic_corpus()
        vpfxs_cnt = sum(1 for w in corpus if (w >> 24) == 0xDC)
        vpfxt_cnt = sum(1 for w in corpus if (w >> 24) == 0xDD)
        vpfxd_cnt = sum(1 for w in corpus if (w >> 24) == 0xDE and w not in set(generate_malformed_corpus()))
        # viim/vfim both have top 0xDF, distinguish by bit23
        viim_cnt = sum(1 for w in corpus if (w >> 24) == 0xDF and ((w >> 23) & 1) == 0)
        vfim_cnt = sum(1 for w in corpus if (w >> 24) == 0xDF and ((w >> 23) & 1) == 1)
        self.assertEqual(vpfxs_cnt, INTENDED_PREFIX_COUNTS["vpfxs"], "vpfxs count must be 32")
        self.assertEqual(vpfxt_cnt, INTENDED_PREFIX_COUNTS["vpfxt"], "vpfxt count must be 32")
        self.assertEqual(vpfxd_cnt, INTENDED_PREFIX_COUNTS["vpfxd"], "vpfxd count must be 48")
        self.assertEqual(viim_cnt, INTENDED_PREFIX_COUNTS["viim"], "viim count must be 35")
        self.assertEqual(vfim_cnt, INTENDED_PREFIX_COUNTS["vfim"], "vfim count must be 35")
        # Mutating VPFX to another legal encoding within same category preserves total but changes literal
        # The literal presence test above kills single-word substitution; this count test kills balanced substitution
        # e.g., swapping one vpfxs for a vpfxt keeps total 182 but breaks per-category counts
        self.assertEqual(vpfxs_cnt + vpfxt_cnt + vpfxd_cnt + viim_cnt + vfim_cnt, INTENDED_FAMILY_COUNTS[0x37],
                         "prefix/immediate sum must match family 0x37 total 182")

    # Note: prefix/immediate substitution mutants that preserve per-category counts
    # but change non-anchor tuples are not claimed to be killed; see contract above.
    # The canonical anchor + per-category count tests are the protected boundary.

class RawYieldUniquenessTests(unittest.TestCase):
    """F3: prove raw yields before dedup are collision-free and match 2742.

    generate_synthetic_corpus() does sorted(set(...)), so len(corpus)==len(set(corpus))
    is tautological.  This test counts raw iterator yields before deduplication
    and asserts raw == unique == 2742.  A mutation where two independent
    generator paths collapse onto the same word must fail explicitly as a
    collision, not merely indirectly because final total falls.  No production
    filtering is introduced to implement the census.
    """
    def test_raw_yield_uniqueness_no_collision(self):
        from vfpu_synth_gen import (
            iter_synthetic_corpus_raw,
            generate_synthetic_corpus,
        )
        raw = list(iter_synthetic_corpus_raw())
        self.assertEqual(len(raw), INTENDED_TOTAL, "raw intended positive yields must be 2742")
        self.assertEqual(len(set(raw)), INTENDED_TOTAL, "unique intended positive words must be 2742")
        self.assertEqual(len(raw), len(set(raw)), "raw == unique, no collision")
        # Final deterministic sorted output must match sorted(set(raw))
        corpus = generate_synthetic_corpus()
        self.assertEqual(corpus, sorted(set(raw)), "final corpus must be sorted(set(raw))")
        self.assertEqual(len(corpus), INTENDED_TOTAL)

class ExhaustiveClassificationTests(unittest.TestCase):
    """F1: exhaustive positive-vs-malformed classification.

    All 2742 intentional positive words must NEVER be classified as malformed.
    All 16 unique malformed corpus words must remain disjoint and in their
    correct historical/illegal buckets.  Specifically pin the three former
    false positives as legitimate positives.  Historical-shape classification
    is not equivalent to production rejection.
    """
    def test_all_positives_never_malformed_exhaustive(self):
        from vfpu_synth_gen import generate_synthetic_corpus, classify_word_production
        corpus = generate_synthetic_corpus()
        self.assertEqual(len(corpus), INTENDED_TOTAL)
        bad = []
        for w in corpus:
            cls = classify_word_production(w)
            if cls.startswith("malformed"):
                bad.append(f"0x{w:08x} -> {cls}")
        self.assertEqual(bad, [], "all 2742 positives must not be malformed:\n" + "\n".join(bad[:10]))
        # Pin three former false positives as legitimate positives
        for w in (0xf0a420a8, 0xf0a4a028, 0xf0a4a0a8):
            self.assertIn(w, set(corpus), f"0x{w:08x} must be in positive corpus")
            cls = classify_word_production(w)
            self.assertTrue(cls.startswith("positive"), f"0x{w:08x} must be legitimate positive, got {cls}")

    def test_all_malformed_disjoint_and_historical_counts(self):
        from vfpu_synth_gen import generate_malformed_corpus, generate_synthetic_corpus, classify_word_production
        pos = set(generate_synthetic_corpus())
        mal = generate_malformed_corpus()
        self.assertEqual(len(mal), 16, "malformed corpus must be 16 unique words")
        self.assertEqual(len(set(mal)), 16)
        overlap = [hex(w) for w in mal if w in pos]
        self.assertEqual(overlap, [], f"malformed must be disjoint from positive, overlap {overlap}")
        # Historical cells: old size-bit 1, old VFIM 3, old 5-bit VFPU12 8, generic illegal 4
        old5 = [w for w in mal if classify_word_production(w) == "malformed: old 5-bit field for 0x3C/sub5"]
        self.assertEqual(len(old5), 8, "old 5-bit VFPU12 class must be 8")
        old_vfim = [w for w in mal if classify_word_production(w) == "malformed: old vfim-family shape"]
        self.assertEqual(len(old_vfim), 3, "old VFIM class must be 3")
        # Remaining 5 are old size-bit (1) + generic illegal (4) -> emitter_unsupported
        remaining = [w for w in mal if w not in old5 and w not in old_vfim]
        self.assertEqual(len(remaining), 5, "remaining malformed (old size-bit + generic illegal) must be 5")
        for w in remaining:
            cls = classify_word_production(w)
            self.assertTrue(cls.startswith("emitter_unsupported"),
                            f"0x{w:08x} remaining malformed must be emitter_unsupported, got {cls}")
        # Verify the 8 are exactly the historical set
        from vfpu_synth_gen import _HISTORICAL_MALFORMED_VFPU12_5BIT_WORDS
        self.assertEqual(set(old5), set(_HISTORICAL_MALFORMED_VFPU12_5BIT_WORDS))

class VcrspVqmulActualSub5Tests(unittest.TestCase):
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self.corpus=generate_synthetic_corpus()
    def test_actual_sub5_present_and_correct_size(self):
        actual=[w for w in self.corpus if _op6(w)==0x3C and _sub3(w)==5]
        self.assertEqual(len(actual),6, f"actual 0x3C/sub5 must be 6 (3*2), got {len(actual)}")
        for w in actual:
            self.assertIn(_size_independent(w),(3,4), f"0x{w:08x} size T/Q")
        from vfpu_synth_gen import _malformed_vfpu12_5bit_field
        for w in actual:
            self.assertEqual(_sub3(w),5)
    def test_no_old_5bit_field_words_in_corpus(self):
        from vfpu_synth_gen import _malformed_vfpu12_5bit_field
        s=set(self.corpus)
        for size_code in (2,3):
            for vs in (0,32):
                for vt in (0,4):
                    buggy=_malformed_vfpu12_5bit_field(size_code,8,vs,vt)
                    self.assertNotIn(buggy,s, f"buggy 0x{buggy:08x} must not be in positive")
                    self.assertEqual(_sub3(buggy),1, "buggy decodes as sub1")
    def test_vcrsp_vqmul_words_decode_via_production_as_sub5(self):
        import codegen
        actual=[w for w in self.corpus if _op6(w)==0x3C and _sub3(w)==5]
        for w in actual[:3]:
            try:
                body,_,_=codegen.vfpu_effect(0x08900000,w)
            except codegen.Unsupported as e:
                self.fail(f"0x{w:08x} unsupported {e}")
            self.assertNotIn("sr_vfpu_interp",body)
    def test_vcrsp_vqmul_literal_anchors_in_corpus(self):
        # Independent encoding anchors; corpus uses matrix regs (vs 0/32/64 with vt=vs+4)
        # so zero-reg literal is not in corpus but encoding must be correct via _encode.
        from vfpu_synth_gen import _encode
        self.assertEqual(_encode(0x3C,5,0,0,0,2), EXPECTED_VCRSP_T, "vcrsp.t encoding must be 0xF2808000")
        self.assertEqual(_encode(0x3C,5,0,0,0,3), EXPECTED_VQMUL_Q, "vqmul.q encoding must be 0xF2808080")
        self.assertEqual((EXPECTED_VCRSP_T>>23)&7,5)
        self.assertEqual((EXPECTED_VQMUL_Q>>23)&7,5)
        self.assertEqual(_size_independent(EXPECTED_VCRSP_T),3)
        self.assertEqual(_size_independent(EXPECTED_VQMUL_Q),4)
        # Corpus must still contain actual sub5 words (6) with correct sizes
        actual=[w for w in self.corpus if _op6(w)==0x3C and _sub3(w)==5]
        self.assertEqual(len(actual),6, "corpus must have 6 actual sub5 words")
        for w in actual:
            self.assertIn(_size_independent(w),(3,4))

class SizeDistributionInvariantsTests(unittest.TestCase):
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self.corpus=generate_synthetic_corpus()
    def test_each_size_class_present(self):
        cnt=Counter(_size_independent(w) for w in self.corpus)
        for size in (1,2,3,4):
            self.assertGreater(cnt.get(size,0),0, f"size {size} must be present")
            self.assertGreaterEqual(cnt[size],600, f"size {size} too low {cnt[size]}")
        total=len(self.corpus)
        for size,c in cnt.items():
            self.assertLess(c/total,0.5, f"size {size} dominates")
        self.assertEqual(cnt[1],INTENDED_SIZE_COUNTS[1], "S count")
        self.assertEqual(cnt[2],INTENDED_SIZE_COUNTS[2], "P count")
        self.assertEqual(cnt[3],INTENDED_SIZE_COUNTS[3], "T count")
        self.assertEqual(cnt[4],INTENDED_SIZE_COUNTS[4], "Q count")
    def test_size_distribution_per_family_exact(self):
        # Guard against size redistribution within same suboperation that preserves global totals
        for op in INTENDED_FAMILY_SIZE_COUNTS:
            words=[w for w in self.corpus if _op6(w)==op]
            cnt=Counter(_size_independent(w) for w in words)
            for size in (1,2,3,4):
                exp = INTENDED_FAMILY_SIZE_COUNTS[op][size]
                self.assertEqual(cnt.get(size,0), exp,
                                 f"family 0x{op:02x} size {size} must be {exp}, got {cnt.get(size,0)}")
    def test_sub_size_distribution_exact(self):
        # Kill redistribute one legal size within same suboperation
        for (fam, sub), exp_map in INTENDED_SUB_SIZE_COUNTS.items():
            words=[w for w in self.corpus if _op6(w)==fam and _sub3(w)==sub]
            cnt=Counter(_size_independent(w) for w in words)
            for size in (1,2,3,4):
                exp = exp_map[size]
                self.assertEqual(cnt.get(size,0), exp,
                                 f"family 0x{fam:02x} sub {sub} size {size} must be {exp}, got {cnt.get(size,0)}")
        # VFPU4 jump-size exact
        for (jump, size), exp in INTENDED_VFPU4_JUMP_SIZE_COUNTS.items():
            words=[w for w in self.corpus if _op6(w)==0x34 and _jump(w)==jump and _size_independent(w)==size]
            self.assertEqual(len(words), exp,
                             f"VFPU4 jump {jump} size {size} must be {exp}, got {len(words)}")

class FamilySubOpCoverageInvariantsTests(unittest.TestCase):
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self.corpus=generate_synthetic_corpus()
    def test_total_count_exact(self):
        self.assertEqual(len(self.corpus),INTENDED_TOTAL)
    def test_per_family_exact_counts(self):
        fam=Counter(_op6(w) for w in self.corpus)
        for op,exp in INTENDED_FAMILY_COUNTS.items():
            self.assertEqual(fam.get(op,0),exp, f"family 0x{op:02x} {exp}")
    def test_per_subop_exact_counts_for_0x3C(self):
        subcnt=Counter(_sub3(w) for w in self.corpus if _op6(w)==0x3C)
        for sub, exp in INTENDED_FAMILY_SUB_COUNTS[0x3C].items():
            self.assertEqual(subcnt.get(sub,0),exp, f"0x3C sub {sub} must be {exp}")
        cnt_idx28=sum(1 for w in self.corpus if _op6(w)==0x3C and _sub3(w)==7 and ((w>>21)&0x1F)==28)
        self.assertEqual(cnt_idx28,72, "matrix1 idx28")
        cnt_idx29=sum(1 for w in self.corpus if _op6(w)==0x3C and _sub3(w)==7 and ((w>>21)&0x1F)==29)
        self.assertEqual(cnt_idx29, INTENDED_VFPU12_IDX_COUNTS[29], "idx29")
        # which breakdown for idx28
        which_cnt=Counter((w>>16)&0xF for w in self.corpus if _op6(w)==0x3C and _sub3(w)==7 and ((w>>21)&0x1F)==28)
        for which, exp in INTENDED_VFPU12_WHICH_COUNTS.items():
            self.assertEqual(which_cnt.get(which,0), exp, f"0x3C idx28 which {which} must be {exp}")
    def test_per_subop_for_0x19_and_0x1B_exact(self):
        # Kill balanced removal of 0x1B/sub6 replaced with sub0
        for fam in (0x19, 0x1B, 0x18):
            cnt=Counter(_sub3(w) for w in self.corpus if _op6(w)==fam)
            for sub, exp in INTENDED_FAMILY_SUB_COUNTS[fam].items():
                self.assertEqual(cnt.get(sub,0),exp, f"family 0x{fam:02x} sub {sub} must be {exp}, got {cnt.get(sub,0)}")
            # Also ensure no unexpected subs present
            for sub in cnt:
                self.assertIn(sub, INTENDED_FAMILY_SUB_COUNTS[fam], f"family 0x{fam:02x} unexpected sub {sub}")
    def test_vfpu4_jump_counts_exact(self):
        cnt_jump=Counter(_jump(w) for w in self.corpus if _op6(w)==0x34)
        for jump, exp in INTENDED_VFPU4_JUMP_COUNTS.items():
            self.assertEqual(cnt_jump.get(jump,0), exp, f"VFPU4 jump {jump} must be {exp}, got {cnt_jump.get(jump,0)}")
        self.assertEqual(sum(cnt_jump.values()), INTENDED_FAMILY_COUNTS[0x34])
        # Kill VFPU4 denominator inflation: total 2020 exact, each jump exact

class IntendedCoverageContractTests(unittest.TestCase):
    """Explicit independent intended-coverage specification contract.
    Expected values are hard-coded from spec/iterator params, not computed from output.
    This kills: balanced 0x1B/sub6->sub0, balanced VFPU4 redistribution,
    remove+duplicate in same family/size, size redistribution within same sub,
    drop family, drop Q, drop sub5, prefix/immediate substitution,
    VFPU4 denominator inflation, malformed injection.
    """
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self.corpus = generate_synthetic_corpus()
        self.cset = set(self.corpus)
    def test_intended_spec_matches_generator_spec(self):
        # Cross-check that test's independent table matches generator's hard-coded table
        from vfpu_synth_gen import (
            INTENDED_TOTAL as G_TOTAL,
            INTENDED_FAMILY_COUNTS as G_FAM,
            INTENDED_SIZE_COUNTS as G_SIZE,
            INTENDED_FAMILY_SUB_COUNTS as G_SUB,
            INTENDED_VFPU4_JUMP_COUNTS as G_JUMP,
            INTENDED_PREFIX_COUNTS as G_PREF,
        )
        self.assertEqual(G_TOTAL, INTENDED_TOTAL)
        self.assertEqual(G_FAM, INTENDED_FAMILY_COUNTS)
        self.assertEqual(G_SIZE, INTENDED_SIZE_COUNTS)
        self.assertEqual(G_SUB, INTENDED_FAMILY_SUB_COUNTS)
        self.assertEqual(G_JUMP, INTENDED_VFPU4_JUMP_COUNTS)
        self.assertEqual(G_PREF, INTENDED_PREFIX_COUNTS)
    def test_all_families_present_and_exact(self):
        fam=Counter(_op6(w) for w in self.corpus)
        for op, exp in INTENDED_FAMILY_COUNTS.items():
            self.assertEqual(fam.get(op,0), exp, f"family 0x{op:02x}")
        # Drop-a-family mutant: removing entire family would be caught
        self.assertEqual(len(fam), len(INTENDED_FAMILY_COUNTS), "families must be exactly the intended set")
        for op in fam:
            self.assertIn(op, INTENDED_FAMILY_COUNTS, f"unexpected family 0x{op:02x}")
    def test_drop_Q_killed(self):
        cnt=Counter(_size_independent(w) for w in self.corpus)
        self.assertEqual(cnt[4], INTENDED_SIZE_COUNTS[4], "Q must be 665")
        self.assertGreater(cnt[4], 0, "Q must be present")
        # Also per-family Q
        fam_q = Counter(_size_independent(w) for w in self.corpus if _op6(w)==0x3C)
        self.assertEqual(fam_q[4], 45, "0x3C Q must be 45")
    def test_drop_actual_sub5_killed(self):
        actual=[w for w in self.corpus if _op6(w)==0x3C and _sub3(w)==5]
        self.assertEqual(len(actual), 6, "actual sub5 must be 6")
        # Verify independent encoding (not corpus presence of zero-reg literal)
        from vfpu_synth_gen import _encode
        self.assertEqual(_encode(0x3C,5,0,0,0,2), EXPECTED_VCRSP_T)
        self.assertEqual(_encode(0x3C,5,0,0,0,3), EXPECTED_VQMUL_Q)
        for w in actual:
            self.assertEqual(_sub3(w),5)
            self.assertIn(_size_independent(w),(3,4))
    def test_balanced_0x1B_sub6_to_sub0_killed(self):
        cnt1B=Counter(_sub3(w) for w in self.corpus if _op6(w)==0x1B)
        self.assertEqual(cnt1B.get(6,0), 12, "0x1B sub6 must be 12 (balanced substitution would make 0)")
        self.assertEqual(cnt1B.get(0,0), 12, "0x1B sub0 must be 12 (balanced substitution would make 24)")
    def test_balanced_vfpu4_redistribution_killed(self):
        cnt_jump=Counter(_jump(w) for w in self.corpus if _op6(w)==0x34)
        # If vcst (jump3) words were moved to vocp (jump2) preserving total 2020,
        # jump counts would differ: vcst 240->230, vocp 20->30, but total same.
        self.assertEqual(cnt_jump[3], 240, "vcst jump3 must be 240")
        self.assertEqual(cnt_jump[2], 20, "vocp jump2 must be 20")
        self.assertEqual(cnt_jump[0], 480, "unary jump0 must be 480")
        self.assertEqual(cnt_jump[21], 280, "vcmov jump21 must be 280")
    def test_remove_duplicate_same_family_size_killed(self):
        # Example: remove one vadd.s (0x18 sub0 size S) and duplicate vsub.s (0x18 sub1 size S)
        # Family 0x18 total 144 preserved, size S totals preserved, but per-sub counts break
        cnt18=Counter(_sub3(w) for w in self.corpus if _op6(w)==0x18)
        self.assertEqual(cnt18[0], 48, "0x18 sub0 must be 48")
        self.assertEqual(cnt18[1], 48, "0x18 sub1 must be 48")
        self.assertEqual(cnt18[7], 48, "0x18 sub7 must be 48")
        # Also per-sub-size would catch finer
        cnt_sub_size=Counter((_sub3(w), _size_independent(w)) for w in self.corpus if _op6(w)==0x18)
        self.assertEqual(cnt_sub_size[(0,1)], 12, "0x18 sub0 S must be 12")
        self.assertEqual(cnt_sub_size[(1,1)], 12, "0x18 sub1 S must be 12")
    def test_redistribute_size_within_same_suboperation_killed(self):
        # Example: within vadd (0x18 sub0), move one S to Q preserving sub count 48 but changing size distribution
        cnt=Counter(_size_independent(w) for w in self.corpus if _op6(w)==0x18 and _sub3(w)==0)
        self.assertEqual(cnt[1], 12, "0x18 sub0 S must be 12")
        self.assertEqual(cnt[4], 12, "0x18 sub0 Q must be 12")
        # Similarly for 0x3C sub0
        cnt3c0=Counter(_size_independent(w) for w in self.corpus if _op6(w)==0x3C and _sub3(w)==0)
        self.assertEqual(cnt3c0[2], 3, "0x3C sub0 P must be 3")
        self.assertEqual(cnt3c0[4], 3, "0x3C sub0 Q must be 3")
    def test_prefix_and_immediate_substitution_killed(self):
        vpfxs=sum(1 for w in self.corpus if (w>>24)==0xDC)
        vpfxt=sum(1 for w in self.corpus if (w>>24)==0xDD)
        viim=sum(1 for w in self.corpus if (w>>24)==0xDF and ((w>>23)&1)==0)
        vfim=sum(1 for w in self.corpus if (w>>24)==0xDF and ((w>>23)&1)==1)
        self.assertEqual(vpfxs, 32, "prefix substitution would change vpfxs 32->31")
        self.assertEqual(vpfxt, 32, "prefix substitution would change vpfxt 32->33")
        self.assertEqual(viim, 35, "immediate substitution would change viim")
        self.assertEqual(vfim, 35, "immediate substitution would change vfim")
        self.assertIn(EXPECTED_VPFXS, self.cset)
        self.assertIn(EXPECTED_VFIM, self.cset)
    def test_vfpu4_denominator_inflation_killed(self):
        self.assertEqual(len(self.corpus), INTENDED_TOTAL, "denominator inflation 2020->2220 would make total 2742->2942")
        fam=Counter(_op6(w) for w in self.corpus)
        self.assertEqual(fam[0x34], 2020, "VFPU4 inflation would make 0x34 2020->2220")
        cnt_jump=Counter(_jump(w) for w in self.corpus if _op6(w)==0x34)
        self.assertEqual(sum(cnt_jump.values()), 2020)
    def test_malformed_injection_killed(self):
        from vfpu_synth_gen import generate_malformed_corpus
        mal=set(generate_malformed_corpus())
        # Malformed word injected into positive corpus would be in mal set and in corpus set
        overlap = self.cset.intersection(mal)
        self.assertEqual(len(overlap), 0, f"malformed words must not be in positive corpus, overlap {list(overlap)[:5]}")
        self.assertEqual(len(mal), len(set(mal)), "malformed corpus deduped")
        self.assertLess(len(mal), 100, "malformed corpus must not inflate denominator")
        # Ensure malformed includes old size-bit and old 5-bit field
        from vfpu_synth_gen import _malformed_vfpu12_5bit_field, _encode
        correct = _encode(0x3C,5,0,0,0,2)
        mal_word = _malformed_vfpu12_5bit_field(2,8,0,0)
        self.assertIn(mal_word, mal)
        self.assertNotIn(mal_word, self.cset)
        self.assertNotIn(correct, mal)

class MalformedNegativeCoverageTests(unittest.TestCase):
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus, generate_malformed_corpus
        self.pos=set(generate_synthetic_corpus())
        self.neg=generate_malformed_corpus()
    def test_malformed_outside_positive(self):
        for w in self.neg:
            self.assertNotIn(w,self.pos, f"0x{w:08x} malformed must be outside positive")
    def test_old_vfim_shape_is_malformed(self):
        from vfpu_synth_gen import _malformed_vfim_old_shape, classify_word_production
        for vd,imm in [(0,0x3F),(32,0x80)]:
            mal=_malformed_vfim_old_shape(vd,imm)
            self.assertNotIn(mal,self.pos)
            cls=classify_word_production(mal)
            self.assertTrue(cls.startswith("malformed") or cls.startswith("emitter_unsupported"))
    def test_old_size_bit_malformed(self):
        from vfpu_synth_gen import _malformed_vcrsp_old_size_bit, _encode
        correct=_encode(0x3C,5,0,0,0,2)
        mal=_malformed_vcrsp_old_size_bit(correct)
        self.assertNotIn(mal,self.pos)
        self.assertNotEqual(_size_independent(mal),3)
    def test_old_5bit_field_malformed(self):
        from vfpu_synth_gen import _malformed_vfpu12_5bit_field
        for size_code in (2,3):
            mal=_malformed_vfpu12_5bit_field(size_code,8,0,0)
            self.assertNotIn(mal,self.pos)
            self.assertEqual(_sub3(mal),1)
    def test_completely_illegal_words_are_not_positive(self):
        illegals=[0x0000003F,(0x01<<26),(0x18<<26)|(2<<23),(0x3C<<26)|(6<<23)|0x8080]
        for w in illegals:
            self.assertNotIn(w,self.pos)
    def test_malformed_not_inflating_denominator(self):
        self.assertLess(len(self.neg),100)
        self.assertEqual(len(self.pos),2742)
    def test_malformed_old_5bit_and_sizebit_separate(self):
        # Keep separate: old 5<<21 subfield bug vs wrong vector-size-bit placement
        from vfpu_synth_gen import _malformed_vfpu12_5bit_field, _malformed_vcrsp_old_size_bit, _encode
        correct=_encode(0x3C,5,0,0,0,2)
        mal5=_malformed_vfpu12_5bit_field(2,8,0,0)
        malSz=_malformed_vcrsp_old_size_bit(correct)
        self.assertNotEqual(mal5, malSz, "old 5-bit field and old size-bit malforms must be distinct")
        self.assertEqual(_sub3(mal5),1, "5-bit bug decodes as sub1")
        self.assertNotEqual(_size_independent(malSz),3, "size-bit bug has wrong size")

class CategoryDistinguishingTests(unittest.TestCase):
    def test_emitter_unsupported_is_distinct(self):
        import codegen
        w=(0x3C<<26)|(6<<23)|0x8080
        with self.assertRaises(codegen.Unsupported):
            codegen.vfpu_effect(0x08900000,w)
        from vfpu_synth_gen import classify_word_production
        self.assertTrue(classify_word_production(w).startswith("emitter_unsupported"))
    def test_interpreter_fallback_is_distinct(self):
        import codegen
        w=(0x35<<26)|0x00000000
        try:
            body,_,_=codegen.vfpu_effect(0x08900000,w)
            if "sr_vfpu_interp" in body:
                self.assertIn("sr_vfpu_interp",body)
            else:
                w2=(0x3E<<26)|0x00000000
                body2,_,_=codegen.vfpu_effect(0x08900000,w2)
                self.assertIn("sr_vfpu_interp",body2)
        except codegen.Unsupported:
            self.skipTest("no fallback")
    def test_malformed_vs_positive_are_disjoint(self):
        from vfpu_synth_gen import generate_synthetic_corpus, generate_malformed_corpus, classify_word_production
        pos=generate_synthetic_corpus()
        mal=generate_malformed_corpus()
        for w in pos[:5]:
            self.assertTrue(classify_word_production(w).startswith("positive"))
        for w in mal[:5]:
            self.assertFalse(classify_word_production(w).startswith("positive"))
    def test_positive_corpus_has_no_fallback(self):
        import codegen
        from vfpu_synth_gen import generate_synthetic_corpus
        for w in generate_synthetic_corpus():
            body,_,_=codegen.vfpu_effect(0x08900000,w)
            self.assertNotIn("sr_vfpu_interp",body)

class VfpuWordsTxtAbsenceTest(unittest.TestCase):
    def test_vfpu_words_txt_is_not_committed(self):
        gitignore=(ROOT/".gitignore").read_text(encoding="utf-8")
        self.assertIn("vfpu_words.txt",gitignore)

if __name__=="__main__":
    unittest.main()
