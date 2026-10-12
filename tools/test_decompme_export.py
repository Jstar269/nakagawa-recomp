#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for tools/decompme_export.py (decompilation track, read-only exporter).

Hermetic tests cover the pure pieces - the minimal MIPS ELF object (built then
parsed back), the base context, and the function-extent bounding. An optional
integration test runs the exporter against the private place_game_here/EBOOT.elf
only after explicit private-title opt-in, checking structural invariants without
printing game bytes.
"""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decompme_export as dx  # noqa: E402
import analyze  # noqa: E402
from test_analyzer_span_scope import write_elf  # noqa: E402

REPO = dx.REPO
EBOOT = REPO / "place_game_here" / "EBOOT.elf"


def _parse_min_obj(obj: bytes) -> dict:
    """Parse the subset of ELF that min_mips_elf_object emits, for verification."""
    assert obj[:4] == b"\x7fELF", "bad ELF magic"
    (e_type, e_machine, _ver, _entry, _phoff, e_shoff, _flags,
     _eh, _phe, _phn, _she, e_shnum, e_shstrndx) = struct.unpack("<HHIIIIIHHHHHH", obj[16:52])

    def shdr(i):
        o = e_shoff + i * 40
        return struct.unpack("<10I", obj[o:o + 40])

    sections = [shdr(i) for i in range(e_shnum)]
    shstr_off = sections[e_shstrndx][4]
    shstr_size = sections[e_shstrndx][5]
    shstr = obj[shstr_off:shstr_off + shstr_size]

    def name_of(nameoff):
        return shstr[nameoff:shstr.index(b"\x00", nameoff)].decode("ascii")

    names = [name_of(s[0]) for s in sections]
    ti = names.index(".text")
    sti = names.index(".symtab")
    stri = names.index(".strtab")
    text = obj[sections[ti][4]:sections[ti][4] + sections[ti][5]]
    symtab = obj[sections[sti][4]:sections[sti][4] + sections[sti][5]]
    strtab = obj[sections[stri][4]:sections[stri][4] + sections[stri][5]]
    st_name, _val, st_size, _info, _other, st_shndx = struct.unpack("<IIIBBH", symtab[16:32])
    symname = strtab[st_name:strtab.index(b"\x00", st_name)].decode("ascii")
    return {"e_type": e_type, "e_machine": e_machine, "little_endian": obj[5] == 1,
            "class32": obj[4] == 1, "text": text, "symname": symname,
            "st_size": st_size, "st_shndx": st_shndx, "text_index": ti}


class TestMinMipsElfObject(unittest.TestCase):
    def test_roundtrip(self):
        body = bytes(range(64)) * 3  # 192 bytes, not 4-aligned count-wise is fine
        obj = dx.min_mips_elf_object(body, name="f_deadbeef")
        p = _parse_min_obj(obj)
        self.assertTrue(p["class32"] and p["little_endian"], "must be 32-bit little-endian")
        self.assertEqual(p["e_type"], 1, "ET_REL")
        self.assertEqual(p["e_machine"], 8, "EM_MIPS")
        self.assertEqual(p["text"], body, ".text must hold the exact function bytes")
        self.assertEqual(p["symname"], "f_deadbeef")
        self.assertEqual(p["st_size"], len(body), "symbol size must equal the function size")
        self.assertEqual(p["st_shndx"], p["text_index"], "symbol must point at .text")

    def test_odd_length_body(self):
        body = b"\x01\x02\x03"  # exercises section-offset alignment
        obj = dx.min_mips_elf_object(body, name="x")
        p = _parse_min_obj(obj)
        self.assertEqual(p["text"], body)
        self.assertEqual(p["st_size"], 3)


class TestContextAndExtent(unittest.TestCase):
    def test_context_has_base_typedefs(self):
        ctx = dx.gen_context_c()
        for t in ("u8", "u16", "u32", "s32", "f32", "f64"):
            self.assertIn(f" {t};", ctx)

    def test_func_extent_bounds_to_next_start(self):
        starts = {0x1000, 0x1100, 0x1200}
        ranges = [(0x1000, 0x2000)]
        self.assertEqual(dx.func_extent(0x1000, starts, ranges), (0x1000, 0x1100))

    def test_func_extent_last_uses_range_end(self):
        starts = {0x1000, 0x1200}
        ranges = [(0x1000, 0x2000)]
        self.assertEqual(dx.func_extent(0x1200, starts, ranges), (0x1200, 0x2000))

    def test_func_extent_out_of_range(self):
        self.assertIsNone(dx.func_extent(0x500, {0x1000}, [(0x1000, 0x2000)]))

    def test_context_emits_known_and_unknown_nid_names(self):
        # sceIoOpen is in the corpus without library restriction (pspsdk-sourced) -> 0x109f50bc
        # 0xdeadbeef is an unknown NID
        nids = [
            ("IoFileMgrForUser", 0x109f50bc),
            ("sceDisplay", 0xdeadbeef),
        ]
        ctx = dx.gen_context_c(nids)
        self.assertIn("/*   IoFileMgrForUser  0x109f50bc  sceIoOpen */", ctx)
        self.assertIn("/*   sceDisplay  0xdeadbeef */", ctx)
        self.assertIn("/* Imported NIDs referenced nearby", ctx)

    def test_context_library_attribution_matching(self):
        # IoFileMgrForKernel_76DA16E3 is attributed to IoFileMgrForKernel in the corpus
        nid = 0x76da16e3
        ctx_match = dx.gen_context_c([("IoFileMgrForKernel", nid)])
        self.assertIn("/*   IoFileMgrForKernel  0x76da16e3  IoFileMgrForKernel_76DA16E3 */", ctx_match)
        # Must not match the name when imported under a different library
        ctx_mismatch = dx.gen_context_c([("IoFileMgrForUser", nid)])
        self.assertIn("/*   IoFileMgrForUser  0x76da16e3 */", ctx_mismatch)
        self.assertNotIn("IoFileMgrForKernel_76DA16E3", ctx_mismatch)

    def test_context_missing_corpus_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing_corpus = Path(tmp) / "nonexistent.json"
            ctx = dx.gen_context_c([("IoFileMgrForUser", 0x109f50bc)], corpus_path=missing_corpus)
            self.assertIn("/*   IoFileMgrForUser  0x109f50bc */", ctx)
            self.assertNotIn("sceIoOpen", ctx)

            corrupt_corpus = Path(tmp) / "corrupt.json"
            corrupt_corpus.write_text("{invalid json", encoding="utf-8")
            ctx_corrupt = dx.gen_context_c([("IoFileMgrForUser", 0x109f50bc)], corpus_path=corrupt_corpus)
            self.assertIn("/*   IoFileMgrForUser  0x109f50bc */", ctx_corrupt)
            self.assertNotIn("sceIoOpen", ctx_corrupt)


class TestFunctionCfg(unittest.TestCase):
    """Source-owned control flow, including code outside the approximate extent."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.elf_path = self.root / "synthetic.elf"
        self.env = mock.patch.dict(os.environ, {"NK_TITLE_EXTRA_EXEC_SPAN": ""})
        self.env.start()
        self.addCleanup(self.env.stop)

    def report(self, words, entries):
        write_elf(self.elf_path, words=words)
        elf = analyze.Elf(str(self.elf_path), base=0)
        return analyze.canonical_cfg_report(
            elf, ranges=[(0x1000, 0x1000 + 4 * len(words))], entries=entries)

    def test_call_and_resume_classifications_are_preserved(self):
        report = self.report([
            0x0C000404, 0,  # jal 0x1010; delay slot
            0x03E00008, 0,  # separately discovered resume entry
            0x03E00008, 0,  # callee
        ], [0x1000, 0x1008, 0x1010])
        view = dx.function_cfg_view(report, 0x1000, 0x1008)
        self.assertEqual(view["schema"], "nakagawa.decomp-function-cfg")
        self.assertEqual(view["owned_outside_extent"], [])
        self.assertEqual([r["address"] for r in view["instructions"]],
                         [0x1000, 0x1004])
        edges = {(r["source"], r["target"], r["kind"]): r["classification"]
                 for r in view["successor_classifications"]}
        self.assertEqual(edges[0x1000, 0x1010, "call"], "inter-function-handoff-dispatch")
        self.assertEqual(edges[0x1000, 0x1008, "fallthrough"], "continuation-resume-entry")
        self.assertEqual(view["continuation_edges"], report["continuation_edges"])
        self.assertEqual(view["ownership_conflicts"], report["ownership_conflicts"])
        self.assertEqual(view["boundary_instructions"], [
            {"address": 0x1008, "classification": "interior-entry", "owners": [0x1008]},
            {"address": 0x1010, "classification": "owned", "owners": [0x1010]}])

    def test_noncontiguous_shared_epilogue_keeps_both_owners(self):
        report = self.report([
            0x08000404, 0,  # first entry jumps over the second entry
            0x08000404, 0,  # second entry jumps to the same epilogue
            0x03E00008, 0,
        ], [0x1000, 0x1008])
        view = dx.function_cfg_view(report, 0x1000, 0x1008)
        self.assertEqual(view["owned_outside_extent"], [0x1010, 0x1014])
        self.assertEqual([r["address"] for r in view["instructions"]],
                         [0x1000, 0x1004, 0x1010, 0x1014])
        self.assertEqual(view["ownership_conflicts"], report["ownership_conflicts"])
        self.assertEqual(view["instructions"][2]["owners"], [0x1000, 0x1008])

    def test_likely_annul_and_unknown_indirect_stay_visible(self):
        report = self.report([
            0x50000003, 0x24020001,  # beql $zero,$zero,0x1010; delay slot
            0x0080F809, 0,           # jalr $a0, unresolved
            0x03E00008, 0,
        ], [0x1000])
        view = dx.function_cfg_view(report, 0x1000, 0x1018)
        self.assertTrue(view["instructions"][1]["delay_slot_annulled_when_not_taken"])
        self.assertEqual(view["unresolved_indirect_edges"], report["unresolved_indirect_edges"])
        self.assertIn("interpreter-fail-closed-boundary",
                      [r["classification"] for r in view["successor_classifications"]])

    def test_unowned_extent_bytes_and_incoming_tail_call_are_visible(self):
        report = self.report([
            0x03E00008, 0, 0x24020007, 0,  # return, then unreachable bytes
            0x08000400, 0,                 # tail jump back to 0x1000
        ], [0x1000, 0x1010])
        view = dx.function_cfg_view(report, 0x1000, 0x1010)
        self.assertEqual(view["extent_not_owned"], [0x1008, 0x100c])
        self.assertEqual([(r["source"], r["target"], r["kind"])
                          for r in view["incoming_edges"]], [(0x1010, 0x1000, "tail-call")])

    def test_foreign_code_inside_the_extent_keeps_its_owner(self):
        report = self.report([
            0x08000404, 0,  # 0x1000: first entry jumps into the selected extent
            0x03E00008, 0,  # 0x1008: selected entry returns at once
            0x0C000408, 0,  # 0x1010: first entry's code calls 0x1020
            0x03E00008, 0,
            0x03E00008, 0,  # 0x1020: callee
        ], [0x1000, 0x1008, 0x1020])
        view = dx.function_cfg_view(report, 0x1008, 0x1020)
        self.assertEqual(view["extent_not_owned"], [0x1010, 0x1014, 0x1018, 0x101c])
        owners = {row["address"]: row["owners"] for row in view["instructions"]}
        self.assertEqual({tuple(owners[a]) for a in view["extent_not_owned"]}, {(0x1000,)})
        call = next(r for r in view["edges"] if r["kind"] == "call")
        self.assertEqual((call["source"], call["target"], owners[call["source"]]),
                         (0x1010, 0x1020, [0x1000]))
        self.assertEqual([(r["source"], r["target"], r["kind"]) for r in view["incoming_edges"]],
                         [(0x1000, 0x1010, "direct-jump")])
        self.assertIn({"address": 0x1000, "classification": "owned", "owners": [0x1000]},
                      view["boundary_instructions"])

    def test_invalid_report_is_rejected(self):
        report = self.report([0x03E00008, 0], [0x1000])
        report["successor_classifications"] = []
        with self.assertRaisesRegex(ValueError, "DECOMP_CFG_INVALID"):
            dx.function_cfg_view(report, 0x1000, 0x1008)

    def test_cli_exports_bound_cfg_without_changing_default_bundle_or_analysis(self):
        write_elf(self.elf_path, words=[0x0C000404, 0, 0x03E00008, 0, 0x03E00008, 0])
        before = analyze.analyze(analyze.Elf(str(self.elf_path), base=0))
        plain, observed, repeated = (self.root / name for name in ("plain", "cfg", "repeat"))
        dx.export_function(str(self.elf_path), 0x1000, 0, plain, "entry")
        with mock.patch("builtins.print"):
            self.assertEqual(dx.main([
                str(self.elf_path), "--function", "0x1000", "--name", "entry",
                "--output", str(observed), "--with-cfg"]), 0)
        dx.export_function(str(self.elf_path), 0x1000, 0, repeated, "entry", with_cfg=True)
        for path in plain.iterdir():
            if path.name != "metadata.json":
                self.assertEqual(path.read_bytes(), (observed / path.name).read_bytes(), path.name)
        plain_meta = json.loads((plain / "metadata.json").read_text())
        cfg_meta = json.loads((observed / "metadata.json").read_text())
        binding = cfg_meta.pop("cfg")
        self.assertEqual(cfg_meta, plain_meta)
        cfg_bytes = (observed / "cfg.json").read_bytes()
        self.assertEqual(binding["sha256"], dx.sha256_hex(cfg_bytes))
        view = json.loads(cfg_bytes)
        self.assertEqual(view["evidence"], {
            "status": "ANALYZER_INFERRED", "entry_basis": "legacy-analyzer-entry-set",
            "source_input_sha256": plain_meta["source_input_sha256"],
            "nakagawa_commit": plain_meta["nakagawa_commit"],
        })
        self.assertEqual(cfg_bytes, (repeated / "cfg.json").read_bytes())
        self.assertFalse((plain / "cfg.json").exists())
        self.assertEqual(before, analyze.analyze(analyze.Elf(str(self.elf_path), base=0)))

    def test_cfg_cli_followed_by_plain_export_removes_stale_cfg(self):
        write_elf(self.elf_path, words=[
            0x0C000404, 0, 0x03E00008, 0, 0x03E00008, 0])
        expected, reused = self.root / "expected", self.root / "reused"
        dx.export_function(str(self.elf_path), 0x1000, 0, expected, "entry")

        with mock.patch("builtins.print"):
            self.assertEqual(dx.main([
                str(self.elf_path), "--function", "0x1000", "--name", "entry",
                "--output", str(reused), "--with-cfg"]), 0)
        self.assertTrue((reused / "cfg.json").is_file())
        self.assertIn("cfg", json.loads((reused / "metadata.json").read_text()))

        with mock.patch("builtins.print"):
            self.assertEqual(dx.main([
                str(self.elf_path), "--function", "0x1000", "--name", "entry",
                "--output", str(reused)]), 0)
        self.assertFalse((reused / "cfg.json").exists())
        self.assertNotIn("cfg", json.loads((reused / "metadata.json").read_text()))
        self.assertEqual(
            {p.name: p.read_bytes() for p in expected.iterdir()},
            {p.name: p.read_bytes() for p in reused.iterdir()},
        )

    def test_reused_output_directory_matches_a_fresh_export(self):
        """Optional outputs of an earlier export must not survive into a later bundle."""
        first, changed = self.root / "first.elf", self.root / "changed.elf"
        write_elf(first, words=[0x0C000404, 0, 0x03E00008, 0, 0x03E00008, 0])
        write_elf(changed, words=[0x0C000404, 0x24020007, 0x03E00008, 0, 0x03E00008, 0])
        real_run = subprocess.run

        def fake_run(cmd, *args, **kwargs):
            # Stand-in MIPS objdumps: one lists target.o by content, one fails.
            if cmd[0] == "objdump-ok":
                digest = dx.sha256_hex(Path(cmd[-1]).read_bytes())
                return subprocess.CompletedProcess(cmd, 0, stdout=f"target.o {digest}\n", stderr="")
            if cmd[0] == "objdump-fails":
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="unsupported\n")
            return real_run(cmd, *args, **kwargs)

        disasm, failing = {"objdump": "objdump-ok"}, {"objdump": "objdump-fails"}
        sequences = {
            "plain then objdump": [(first, {}), (first, disasm)],
            "objdump then plain": [(first, disasm), (first, {})],
            "objdump then failing objdump": [(first, disasm), (first, failing)],
            "objdump then plain on changed input": [(first, disasm), (changed, {})],
            "cfg and objdump then cfg on changed input": [
                (first, {**disasm, "with_cfg": True}), (changed, {"with_cfg": True})],
            "cfg then plain on changed input": [(first, {"with_cfg": True}), (changed, {})],
        }
        with mock.patch.object(dx.subprocess, "run", side_effect=fake_run):
            for index, (label, steps) in enumerate(sequences.items()):
                with self.subTest(label):
                    reused, fresh = self.root / f"reused{index}", self.root / f"fresh{index}"
                    for elf, options in steps:
                        dx.export_function(str(elf), 0x1000, 0, reused, "entry", **options)
                    elf, options = steps[-1]
                    dx.export_function(str(elf), 0x1000, 0, fresh, "entry", **options)
                    self.assertEqual(
                        {p.name: p.read_bytes() for p in reused.iterdir()},
                        {p.name: p.read_bytes() for p in fresh.iterdir()},
                    )

    def test_cfg_export_does_not_change_generated_aot_bytes(self):
        write_elf(self.elf_path, words=[0x0C000404, 0, 0x03E00008, 0, 0x03E00008, 0])
        source_bytes = self.elf_path.read_bytes()

        def generate(directory):
            directory.mkdir()
            result = subprocess.run([
                sys.executable, str(REPO / "tools" / "codegen.py"), str(self.elf_path),
                str(directory / "synthetic.c"), "--base=0", "--profile=none",
                "--funcs-per-chunk=2000",
            ], capture_output=True, text=True, timeout=60, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            files = {str(p.relative_to(directory)): p.read_bytes()
                     for p in directory.rglob("*") if p.is_file()}
            self.assertIn("synthetic.c", files)
            return files

        before = generate(self.root / "aot-before")
        dx.export_function(str(self.elf_path), 0x1000, 0, self.root / "bundle", "entry", with_cfg=True)
        self.assertEqual(source_bytes, self.elf_path.read_bytes())
        self.assertEqual(before, generate(self.root / "aot-after"))

    def test_bad_cfg_fails_before_writing_bundle(self):
        write_elf(self.elf_path)
        report = self.report([0x03E00008, 0], [0x1000])
        report["successor_classifications"] = []
        output = self.root / "rejected"
        with mock.patch.object(analyze, "canonical_cfg_report", return_value=report):
            with self.assertRaisesRegex(ValueError, "DECOMP_CFG_INVALID"):
                dx.export_function(str(self.elf_path), 0x1000, 0, output, "entry", with_cfg=True)
        self.assertFalse(output.exists())


@unittest.skipUnless(
    os.environ.get("NK_PRIVATE_TITLE_TESTS") == "1" and EBOOT.is_file(),
    "set NK_PRIVATE_TITLE_TESTS=1 and provide private place_game_here/EBOOT.elf",
)
class TestExportAgainstRealEboot(unittest.TestCase):
    """Structure-only end-to-end check on the real game input. Never prints bytes."""

    def test_bundle_structure(self):
        sys.path.insert(0, dx.TOOLS_DIR)
        import analyze
        elf = analyze.Elf(str(EBOOT), base=0)
        starts, ranges = analyze.analyze(elf)
        # Pick a real function start that has readable bytes.
        addr = None
        for s in sorted(starts):
            ext = dx.func_extent(s, starts, ranges)
            if ext and elf.read_at_vaddr(s, min(64, ext[1] - ext[0])):
                addr = s
                break
        self.assertIsNotNone(addr, "no readable function start found")

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "f"
            meta = dx.export_function(str(EBOOT), addr, 0, out, name=f"f_{addr:08x}")
            # Files exist.
            for fn in ("metadata.json", "context.c", "function.bin", "target.o", "starter.c"):
                self.assertTrue((out / fn).exists(), f"missing {fn}")
            # Metadata is internally consistent with the emitted bytes.
            fb = (out / "function.bin").read_bytes()
            self.assertEqual(len(fb), meta["size"])
            self.assertEqual(dx.sha256_hex(fb), meta["sha256"])
            # target.o embeds exactly those bytes.
            p = _parse_min_obj((out / "target.o").read_bytes())
            self.assertEqual(p["text"], fb)
            self.assertEqual(p["e_machine"], 8)
            # metadata.json is valid JSON with a commit field.
            m = json.loads((out / "metadata.json").read_text())
            self.assertIn("nakagawa_commit", m)


if __name__ == "__main__":
    unittest.main()
