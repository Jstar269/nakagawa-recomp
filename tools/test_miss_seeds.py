# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Dispatch-miss seeds (#1848): a run's miss targets fed back into analysis.

The fixture is a synthetic ELF32/MIPS executable written here. Its entry function saves ``$ra``,
loads a code pointer from memory (``lw $t0, 0($a0)``), calls it (``jalr``) and returns. The
pointer's target is an ``addu`` function that no direct edge, address materialization or return
epilogue names: it sits after two ``break`` words, which the analyzer's trace falls through, so
the first build owns its words only as the interior of the function before it and the dispatcher
has no body to run at that address. The end-to-end test builds the generated C with the real
runtime, runs it, feeds the file the run wrote back to a second build, and checks the second run
no longer misses (the run also exits with the same result both times).
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import analyze  # noqa: E402
import codegen  # noqa: E402
import miss_seeds  # noqa: E402
from test_codegen_entry_semantics import _synthetic_elf  # noqa: E402

ENTRY = 0x00010000
TARGET = 0x00010028
BREAK = 0x0000000D
WORDS = {
    ENTRY: 0x27BDFFF0,           # addiu sp, sp, -16
    ENTRY + 4: 0xAFBF0000,       # sw ra, 0(sp)
    ENTRY + 8: 0x8C880000,       # lw t0, 0(a0): the computed target, unknown to the analyzer
    ENTRY + 12: 0x0100F809,      # jalr ra, t0
    ENTRY + 16: 0x00000000,      # nop (delay slot)
    ENTRY + 20: 0x8FBF0000,      # lw ra, 0(sp)
    ENTRY + 24: 0x03E00008,      # jr ra
    ENTRY + 28: 0x27BD0010,      # addiu sp, sp, 16 (delay slot)
    ENTRY + 32: BREAK,           # padding the trace falls through, so TARGET is interior
    ENTRY + 36: BREAK,
    TARGET: 0x00A61021,          # addu v0, a1, a2
    TARGET + 4: 0x03E00008,      # jr ra
    TARGET + 8: 0x00000000,      # nop (delay slot)
}
RESERVED = 0x74786574            # ASCII "text": opcode 0x1d, which no instruction decodes
# A wild pointer into data: the word at TARGET is not code at all.
RESERVED_WORDS = {**WORDS, TARGET: RESERVED}
# A valid first word whose run path returns normally, with a reserved word on the side the
# walk covers but a run never executes: the entry cannot be translated, yet the interpreter
# floor runs it correctly.
UNEXECUTED_RESERVED_WORDS = {
    **{a: w for a, w in WORDS.items() if a < TARGET},
    TARGET: 0x00A61021,          # addu v0, a1, a2
    TARGET + 4: 0x14A00003,      # bne a1, zero, TARGET+20 (taken: a1 = 40)
    TARGET + 8: 0x00000000,      # nop (delay slot)
    TARGET + 12: RESERVED,       # never executed
    TARGET + 16: 0x00000000,     # nop
    TARGET + 20: 0x03E00008,     # jr ra
    TARGET + 24: 0x00000000,     # nop (delay slot)
}
PADDING_FUNCTION = ENTRY + 32    # the analyzer starts a function at the padding after the return
# The same shape with a decodable word there, which the image lists as a variable-import site:
# the walk covers a word the emitter refuses only under the image's address space.
IMPORT_SITE = TARGET + 12
IMPORT_SITE_WORDS = {**UNEXECUTED_RESERVED_WORDS, IMPORT_SITE: 0x00000000}
# A seed that translates and yet brings a stub. The data word before the seed decodes as a branch,
# so the unseeded trace steps over the `jal` as that branch's delay slot and never sees its target;
# the seeded trace does, and the new entry walks a reserved word the run never executes.
CALLEE = TARGET + 24
CALLEE_STUB_WORDS = {
    **{a: w for a, w in WORDS.items() if a < TARGET},
    ENTRY + 16: 0x03E0C821,      # addu t9, ra, zero (delay slot): keeps the return address
    ENTRY + 36: 0x14A00002,      # data that decodes as bne a1, zero, TARGET+8
    TARGET: 0x0C004010,          # jal CALLEE
    TARGET + 4: 0x00000000,      # nop (delay slot)
    TARGET + 8: 0x0320F821,      # addu ra, t9, zero
    TARGET + 12: 0x03E00008,     # jr ra
    TARGET + 16: 0x00000000,     # nop (delay slot)
    TARGET + 20: BREAK,
    CALLEE: 0x00A61021,          # addu v0, a1, a2
    CALLEE + 4: 0x14A00003,      # bne a1, zero, CALLEE+20 (taken: a1 = 40)
    CALLEE + 8: 0x00000000,      # nop (delay slot)
    CALLEE + 12: RESERVED,       # never executed
    CALLEE + 16: 0x00000000,     # nop
    CALLEE + 20: 0x03E00008,     # jr ra
    CALLEE + 24: 0x00000000,     # nop (delay slot)
}
# A miss whose body reaches a word inside its own frame. The seed list names that word, not the
# miss: it has a valid first instruction and a translatable body, but it releases a frame it never
# allocated.
MID_FRAME = TARGET + 24
MID_FRAME_WORDS = {
    **{a: w for a, w in WORDS.items() if a < TARGET},
    TARGET: 0x27BDFFF0,          # addiu sp, sp, -16
    TARGET + 4: 0xAFBF0000,      # sw ra, 0(sp)
    TARGET + 8: 0x14A00003,      # bne a1, zero, MID_FRAME (taken: a1 = 40)
    TARGET + 12: 0x00000000,     # nop (delay slot)
    TARGET + 16: RESERVED,       # never executed
    TARGET + 20: 0x00000000,     # nop
    MID_FRAME: 0x00A61021,       # addu v0, a1, a2
    MID_FRAME + 4: 0x8FBF0000,   # lw ra, 0(sp)
    MID_FRAME + 8: 0x03E00008,   # jr ra
    MID_FRAME + 12: 0x27BD0010,  # addiu sp, sp, 16 (delay slot)
}
# Interior words the frame walk cannot prove: one that leaves only through a computed jump, with
# no return to read a frame from; one that returns on one path and releases a frame just before
# such a jump on the other; and one that reloads $sp from memory, as longjmp does. The word before
# each is the analyzer's gap entry.
ADDU = 0x00A61021                # addu v0, a1, a2
UNPROVEN_TAIL = TARGET + 16
RELEASED_TAIL = TARGET + 32
RELOADED_SP = TARGET + 60
UNPROVEN_WORDS = {
    **WORDS,
    TARGET + 12: ADDU,
    UNPROVEN_TAIL: ADDU,
    UNPROVEN_TAIL + 4: 0x01000008,    # jr t0
    UNPROVEN_TAIL + 8: 0x00000000,    # nop (delay slot)
    RELEASED_TAIL - 4: ADDU,
    RELEASED_TAIL: 0x10A00003,        # beq a1, zero, RELEASED_TAIL+16
    RELEASED_TAIL + 4: 0x00000000,    # nop (delay slot)
    RELEASED_TAIL + 8: 0x01000008,    # jr t0
    RELEASED_TAIL + 12: 0x27BD0010,   # addiu sp, sp, 16 (delay slot)
    RELEASED_TAIL + 16: 0x03E00008,   # jr ra
    RELEASED_TAIL + 20: 0x00000000,   # nop (delay slot)
    RELOADED_SP - 4: ADDU,
    RELOADED_SP: ADDU,
    RELOADED_SP + 4: 0x8C9D0004,      # lw sp, 4(a0)
    RELOADED_SP + 8: 0x03E00008,      # jr ra
    RELOADED_SP + 12: 0x00000000,     # nop (delay slot)
}
# A synthetic setjmp/longjmp. E calls F; F builds a frame and calls setjmp; the second return from
# setjmp, by way of longjmp, lands on CONTINUATION, the word after the setjmp call, in F's frame.
F = ENTRY + 0x1C
CONTINUATION = F + 0x10
DONE = CONTINUATION + 0x10
SETJMP = DONE + 0x10
LONGJMP = SETJMP + 0x10
LONGJMP_WORDS = {
    ENTRY: 0x27BDFFF0,           # addiu sp, sp, -16
    ENTRY + 4: 0xAFBF0000,       # sw ra, 0(sp)
    ENTRY + 8: 0x0C000000 | F >> 2,    # jal F
    ENTRY + 12: 0x00000000,      # nop (delay slot)
    ENTRY + 16: 0x8FBF0000,      # lw ra, 0(sp)
    ENTRY + 20: 0x03E00008,      # jr ra
    ENTRY + 24: 0x27BD0010,      # addiu sp, sp, 16 (delay slot)
    F: 0x27BDFFF0,               # addiu sp, sp, -16
    F + 4: 0xAFBF0000,           # sw ra, 0(sp)
    F + 8: 0x0C000000 | SETJMP >> 2,   # jal SETJMP
    F + 12: 0x00000000,          # nop (delay slot)
    CONTINUATION: 0x14400003,    # bne v0, zero, DONE (taken on the return from longjmp)
    CONTINUATION + 4: 0x00000000,      # nop (delay slot)
    CONTINUATION + 8: 0x0C000000 | LONGJMP >> 2,  # jal LONGJMP
    CONTINUATION + 12: 0x00000000,     # nop (delay slot)
    DONE: 0x00A61021,            # addu v0, a1, a2
    DONE + 4: 0x8FBF0000,        # lw ra, 0(sp)
    DONE + 8: 0x03E00008,        # jr ra
    DONE + 12: 0x27BD0010,       # addiu sp, sp, 16 (delay slot)
    SETJMP: 0xAC9F0000,          # sw ra, 0(a0)
    SETJMP + 4: 0xAC9D0004,      # sw sp, 4(a0)
    SETJMP + 8: 0x03E00008,      # jr ra
    SETJMP + 12: 0x00001021,     # addu v0, zero, zero (delay slot)
    LONGJMP: 0x8C9F0000,         # lw ra, 0(a0)
    LONGJMP + 4: 0x8C9D0004,     # lw sp, 4(a0)
    LONGJMP + 8: 0x03E00008,     # jr ra
    LONGJMP + 12: 0x24020001,    # addiu v0, zero, 1 (delay slot)
}
# Runs main with the image's variable-import sites replaced: no fixture builds an import table.
WITH_IMPORT_SITE = """
import sys
sys.path.insert(0, sys.argv[1])
import codegen, {module} as tool
codegen.variable_import_sites = lambda elf: ((), {{{site}: "word32"}})
sys.exit(tool.main(sys.argv[1:]))
"""

# The harness mirrors the cosimulation gate's (fixtures/cosim/cosim_selftest.c): it includes the
# production dispatch core, supplies the few host symbols a build without the scheduler and GE
# lacks, and enters the module through dispatch_from_host. Every other runtime file is real.
HARNESS = r'''
#include "recomp.c"
#include "perf.h"

#include <stdlib.h>
#include <string.h>

void sr_register_all(void);

volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;
CpuState *s_cpu = NULL;
int sr_sched_on = 0;
atomic_int_least32_t sr_timeslice;

uint32_t sched_current_uid(void) { return 0u; }
void sched_exit_current(int32_t status) { (void)status; }
void sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void sched_set_current_join_target(uint32_t uid) { (void)uid; }
void sched_clear_current_join_target(void) {}
int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t sr_hle_resolve_late_import(uint32_t nid) { (void)nid; return 0u; }
void sr_main_module_register(const SrMainModuleRecord *record) { (void)record; }
uint32_t sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; return 0u; }
void sr_yield(CpuState *s) { (void)s; }
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: harness IMAGE BASE ENTRY TARGET\n");
        return 2;
    }
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 16);
    uint32_t entry = (uint32_t)strtoul(argv[3], NULL, 16);
    uint32_t target = (uint32_t)strtoul(argv[4], NULL, 16);
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 2;
    static unsigned char bytes[4096];
    size_t size = fread(bytes, 1, sizeof bytes, fp);
    fclose(fp);
    sr_mem_init();
    sr_load_segment(base, bytes, (uint32_t)size);
    sr_register_all();

    enum { POINTER_CELL = 0x08900000u };
    MEM_W32(POINTER_CELL, target);
    CpuState s;
    memset(&s, 0, sizeof s);
    s.r[4] = POINTER_CELL;   /* $a0: the cell holding the computed call target */
    s.r[5] = 40u;            /* $a1, $a2: operands of the function the call reaches */
    s.r[6] = 2u;
    s.r[29] = 0x08A00000u;
    s.r[31] = SR_HOST_RETURN_PC;
    s_cpu = &s;
    sr_exec_share_arm();
    dispatch_from_host(&s, entry);
    printf("RESULT v0=%u\n", (unsigned)s.r[2]);
    sr_exec_share_report();
    return s.r[2] == 42u ? 0 : 1;
}
'''

RUNTIME_SOURCES = (
    "guest_interp.c", "sr_coro.c", "flight_recorder.c", "cpu_lle.c", "domain_mode.c",
    "stale_code.c", "title_config.c", "vfpu_tables.c", "perf.c",
)


def _image_bytes(words):
    return b"".join(struct.pack("<I", words.get(a, 0))
                    for a in range(ENTRY, max(words) + 4, 4))


def _codegen(elf_path, out_c, *extra):
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "codegen.py"), str(elf_path), str(out_c),
         "--funcs-per-chunk=1", *extra],
        cwd=ROOT, capture_output=True, text=True,
    )


def _functions(elf_path, **kwargs):
    starts, _ranges = analyze.analyze(analyze.Elf(str(elf_path)), **kwargs)
    return starts


def _emission(stubs):
    """A ``seed_check``: ``stubs(entries)`` names the entries the emitter stubs, with why."""
    def check(entries, ranges, only=None):
        stubbed = stubs(set(entries))
        return {a: stubbed.get(a) for a in entries if only in (None, a)}
    return check


def _with_import_site(module, *argv):
    return subprocess.run(
        [sys.executable, "-c", WITH_IMPORT_SITE.format(module=module, site=IMPORT_SITE),
         str(ROOT / "tools"), *argv],
        cwd=ROOT, capture_output=True, text=True,
    )


class _Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="miss_seeds_")
        cls.work = Path(cls._tmp.name)
        cls.elf_path = cls.work / "miss.elf"
        cls.elf_path.write_bytes(_synthetic_elf(WORDS, ENTRY))

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def write_seeds(self, name, text):
        path = self.work / name
        path.write_text(text, encoding="ascii", newline="\n")
        return path


class SeedFileTests(unittest.TestCase):
    def test_a_record_may_carry_a_run_label_and_the_runtime_writes_none(self):
        parsed = miss_seeds.parse_seed_text(
            "# nakagawa-miss-seeds-v1\n# miss_targets=2 entry_points_overflow=0\n"
            "0x00010028\n0x08804068 run=smoke-1\n\n")
        self.assertEqual(parsed.seeds, {0x00010028: "unknown", 0x08804068: "smoke-1"})
        self.assertEqual(parsed.overflow, 0)

    def test_overflow_is_read_from_the_runtime_comment(self):
        parsed = miss_seeds.parse_seed_text("# miss_targets=0 entry_points_overflow=7\n")
        self.assertEqual(parsed.overflow, 7)

    def test_a_malformed_record_is_refused_with_its_line(self):
        with self.assertRaisesRegex(miss_seeds.SeedFileError, r"seeds.txt:2: not a seed record"):
            miss_seeds.parse_seed_text("0x00010028\nbanana\n", "seeds.txt")

    def test_format_is_sorted_and_round_trips(self):
        text = miss_seeds.format_seed_file({0x20: "b", 0x10: "a"})
        self.assertEqual(text, "# nakagawa-miss-seeds-v1\n0x00000010 run=a\n0x00000020 run=b\n")
        self.assertEqual(miss_seeds.parse_seed_text(text).seeds, {0x10: "a", 0x20: "b"})

    def test_merge_keeps_the_first_run_and_drops_known_entries(self):
        existing = miss_seeds.SeedFile({0x10: "first"})
        new = miss_seeds.SeedFile({0x10: "unknown", 0x20: "unknown", 0x30: "unknown"}, overflow=2)
        seeds, added, dropped, overflow = miss_seeds.merge(
            existing, [new], "second", known=frozenset({0x30}))
        self.assertEqual(seeds, {0x10: "first", 0x20: "second"})
        self.assertEqual((added, dropped, overflow), ([0x20], [0x30], 2))


class AnalysisSeedTests(_Fixture):
    def test_without_seeds_the_target_is_not_a_function_entry(self):
        starts = _functions(self.elf_path)
        self.assertEqual(sorted(starts), [ENTRY, PADDING_FUNCTION])
        self.assertNotIn(TARGET, starts)

    def test_no_seeds_and_an_empty_seed_set_give_the_same_analysis(self):
        self.assertEqual(_functions(self.elf_path), _functions(self.elf_path, seeds=set()))

    def test_a_seed_becomes_a_function_entry_and_reports_applied(self):
        report: dict = {}
        starts = _functions(self.elf_path, seeds={TARGET}, report=report)
        self.assertIn(TARGET, starts)
        self.assertEqual(report["miss_seeds"], {TARGET: analyze.SEED_APPLIED})

    def test_a_seed_that_is_already_an_entry_is_known(self):
        report: dict = {}
        starts = _functions(self.elf_path, seeds={ENTRY}, report=report)
        self.assertEqual(report["miss_seeds"], {ENTRY: analyze.SEED_KNOWN})
        self.assertEqual(sorted(starts), [ENTRY, PADDING_FUNCTION])

    def test_a_seed_that_cannot_start_a_function_is_reported_and_ignored(self):
        bad = {
            TARGET + 2: analyze.SEED_UNALIGNED,
            0x00020000: analyze.SEED_OUTSIDE_CODE,
            0x00010034: analyze.SEED_NOT_CODE,   # segment padding past the last word
        }
        report: dict = {}
        starts = _functions(self.elf_path, seeds=set(bad), report=report)
        self.assertEqual(report["miss_seeds"], bad)
        self.assertEqual(sorted(starts), [ENTRY, PADDING_FUNCTION])

    def test_an_all_zero_word_is_not_a_function_start(self):
        words = dict(WORDS)
        words[TARGET + 8] = 0
        path = self.work / "zero.elf"
        path.write_bytes(_synthetic_elf(words, ENTRY))
        report: dict = {}
        _functions(path, seeds={TARGET + 8}, report=report)
        self.assertEqual(report["miss_seeds"], {TARGET + 8: analyze.SEED_NOT_CODE})

    def test_a_seed_the_emitter_stubs_is_ignored_with_its_reason_and_adds_no_entry(self):
        check = _emission(lambda entries: {TARGET: "no thanks"})
        report: dict = {}
        starts = _functions(self.elf_path, seeds={TARGET}, report=report, seed_check=check)
        self.assertEqual(report["miss_seeds"],
                         {TARGET: f"{analyze.SEED_UNTRANSLATABLE}: no thanks"})
        self.assertEqual(sorted(starts), [ENTRY, PADDING_FUNCTION])

    def test_a_seed_on_a_gap_filled_entry_is_known_and_adds_nothing(self):
        unseeded: dict = {}
        _functions(self.elf_path, report=unseeded)
        self.assertNotIn(PADDING_FUNCTION, unseeded["hc"])
        report: dict = {}
        starts = _functions(self.elf_path, seeds={PADDING_FUNCTION}, report=report)
        self.assertEqual(report["miss_seeds"], {PADDING_FUNCTION: analyze.SEED_KNOWN})
        self.assertEqual(starts, _functions(self.elf_path))

    def test_a_stub_the_build_has_without_seeds_is_not_laid_on_the_seed(self):
        check = _emission(lambda entries: {PADDING_FUNCTION: "stubbed anyway"})
        report: dict = {}
        starts = _functions(self.elf_path, seeds={PADDING_FUNCTION}, report=report,
                            seed_check=check)
        self.assertEqual(report["miss_seeds"], {PADDING_FUNCTION: analyze.SEED_KNOWN})
        self.assertEqual(sorted(starts), [ENTRY, PADDING_FUNCTION])

    def test_a_seed_that_turns_an_existing_entry_into_a_stub_is_refused(self):
        check = _emission(lambda entries: {ENTRY: "split"} if TARGET in entries else {})
        report: dict = {}
        starts = _functions(self.elf_path, seeds={TARGET}, report=report, seed_check=check)
        self.assertEqual(report["miss_seeds"],
                         {TARGET: f"{analyze.SEED_UNTRANSLATABLE}: brings 0x{ENTRY:08x}: split"})
        self.assertEqual(sorted(starts), [ENTRY, PADDING_FUNCTION])

    def test_a_seed_that_is_not_emitted_is_not_applied(self):
        check = _emission(lambda entries: {})

        def without_target(entries, ranges, only=None):
            return {a: why for a, why in check(entries, ranges, only).items() if a != TARGET}

        report: dict = {}
        starts = _functions(self.elf_path, seeds={TARGET}, report=report,
                            seed_check=without_target)
        self.assertEqual(report["miss_seeds"], {TARGET: analyze.SEED_NOT_EMITTED})
        self.assertIn(TARGET, starts)

    def test_only_the_seed_that_brings_a_stub_is_refused(self):
        elf = self.work / "callee_stub.elf"
        elf.write_bytes(_synthetic_elf(CALLEE_STUB_WORDS, ENTRY))
        self.assertNotIn(CALLEE, _functions(elf))
        bystanders = {TARGET + 12, CALLEE + 20}
        report: dict = {}
        starts = _functions(elf, seeds={TARGET, *bystanders}, report=report,
                            seed_check=codegen.emitter_seed_check(analyze.Elf(str(elf))))
        self.assertEqual(report["miss_seeds"], {
            TARGET + 12: analyze.SEED_APPLIED,
            TARGET: f"{analyze.SEED_UNTRANSLATABLE}: brings 0x{CALLEE:08x}: "
                    f"opcode 0x1d at 0x{CALLEE + 12:08x}",
            CALLEE + 20: analyze.SEED_APPLIED,
        })
        self.assertNotIn(CALLEE, starts)
        self.assertLessEqual(bystanders, starts)

    def test_a_seed_that_is_not_a_function_entry_is_refused_with_why(self):
        unproven = self.work / "unproven.elf"
        unproven.write_bytes(_synthetic_elf(UNPROVEN_WORDS, ENTRY))
        mid_frame = self.work / "mid_frame.elf"
        mid_frame.write_bytes(_synthetic_elf(MID_FRAME_WORDS, ENTRY))
        longjmp = self.work / "longjmp.elf"
        longjmp.write_bytes(_synthetic_elf(LONGJMP_WORDS, ENTRY))
        for elf, seed, why in (
                (self.elf_path, ENTRY + 20, "continuation: $sp net +16 at a return"),
                (mid_frame, MID_FRAME, "continuation: $sp net +16 at a return"),
                (longjmp, CONTINUATION, "continuation: $sp net +16 at a return"),
                (unproven, UNPROVEN_TAIL, "indeterminate"),
                (unproven, RELEASED_TAIL, "continuation: $sp net +16 at a computed jump"),
                (unproven, RELOADED_SP, "indeterminate")):
            with self.subTest(hex(seed)):
                report: dict = {}
                starts = _functions(elf, seeds={seed}, report=report)
                self.assertEqual(report["miss_seeds"],
                                 {seed: f"{analyze.SEED_NOT_CALLABLE}: {why}"})
                self.assertEqual(starts, _functions(elf))

    def test_a_word_inside_a_body_with_no_frame_live_is_still_a_seed(self):
        report: dict = {}
        starts = _functions(self.elf_path, seeds={TARGET + 4}, report=report)
        self.assertEqual(report["miss_seeds"], {TARGET + 4: analyze.SEED_APPLIED})
        self.assertIn(TARGET + 4, starts)

    def test_the_report_keeps_the_discovery_state_of_the_build_it_describes(self):
        report: dict = {}
        _functions(self.elf_path, seeds={TARGET}, report=report,
                   seed_check=_emission(lambda entries: {TARGET: "no"}))
        self.assertNotIn(TARGET, report["hc"])

    def test_an_import_stub_with_no_nid_counts_as_a_stub(self):
        # The emission loop turns such an entry into an abort, as it does an untranslatable body.
        image = analyze.Elf(str(self.elf_path))
        section = {"addr": TARGET, "size": 12}
        sections = image.sec
        image.sec = lambda name: section if name == ".lib.stub" else sections(name)
        starts, ranges = analyze.analyze(image, seeds={TARGET})
        emission = codegen.emitter_seed_check(image)(starts, ranges)
        self.assertEqual(emission[TARGET], "import stub without NID mapping")
        self.assertIsNone(emission[ENTRY])

    def test_the_analyze_command_takes_a_seed_list(self):
        seeds = self.write_seeds("analyze_seeds.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        toml = self.work / "seeded.toml"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "analyze.py"), str(self.elf_path),
             f"--miss-seeds={seeds}", f"--toml={toml}", "--quiet"],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MISS_SEEDS_APPLIED seeds=1 applied=1 known=0 ignored=0", result.stdout)
        self.assertIn("0x00010028", toml.read_text(encoding="ascii"))

    def test_the_analyze_command_checks_seeds_under_the_images_address_space(self):
        elf = self.work / "import_site_analyze.elf"
        elf.write_bytes(_synthetic_elf(IMPORT_SITE_WORDS, ENTRY))
        seeds = self.write_seeds("import_site_analyze.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        result = _with_import_site("analyze", str(elf), f"--miss-seeds={seeds}",
                                   f"--toml={self.work / 'import_site.toml'}", "--quiet")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MISS_SEEDS_APPLIED seeds=1 applied=0 known=0 ignored=1", result.stdout)
        self.assertIn(f"ignored 0x00010028: untranslatable: variable-import word32 reference "
                      f"on an executed word at 0x{IMPORT_SITE:08x}", result.stderr)


class MergeCommandTests(_Fixture):
    def test_merge_adds_a_target_inside_a_body_and_drops_a_known_entry(self):
        miss = self.write_seeds(
            "miss.txt", "# nakagawa-miss-seeds-v1\n0x00010000\n0x00010028\n")
        out = self.work / "title_seeds.txt"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "miss_seeds.py"), "merge", f"--out={out}",
             "--run=run-1", f"--elf={self.elf_path}", str(miss)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("added=1 dropped_known=1 total=1", result.stdout)
        self.assertEqual(out.read_text(encoding="ascii"),
                         "# nakagawa-miss-seeds-v1\n0x00010028 run=run-1\n")
        # A second merge keeps the run that found the seed.
        again = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "miss_seeds.py"), "merge", f"--out={out}",
             "--run=run-2", str(miss)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn("run=run-1", out.read_text(encoding="ascii"))
        self.assertIn("0x00010000 run=run-2", out.read_text(encoding="ascii"))

    def test_an_overflowing_run_is_called_incomplete(self):
        miss = self.write_seeds(
            "overflow.txt", "# miss_targets=1 entry_points_overflow=3\n0x00010028\n")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "miss_seeds.py"), "merge",
             f"--out={self.work / 'overflow_seeds.txt'}", "--run=r", str(miss)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3 interpreter entries that did not fit", result.stderr)

    def test_a_malformed_seed_list_is_refused(self):
        miss = self.write_seeds("bad.txt", "not a seed\n")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "miss_seeds.py"), "merge",
             f"--out={self.work / 'never.txt'}", "--run=r", str(miss)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.work / "never.txt").exists())


class CodegenSeedTests(_Fixture):
    def build(self, name, *extra):
        out = self.work / name
        out.mkdir(exist_ok=True)
        result = _codegen(self.elf_path, out / "m_recomp.c", *extra)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return out, result

    @staticmethod
    def functions_line(result):
        line = next(line for line in result.stdout.splitlines()
                    if line.startswith("CODEGEN_COVERAGE "))
        return int(dict(part.split("=") for part in line.split()[1:])["functions"])

    @staticmethod
    def stubs(result):
        return [line for line in result.stderr.splitlines() if line.startswith("skip 0x")]

    def test_the_default_output_is_unchanged_by_an_empty_seed_list(self):
        plain, plain_result = self.build("plain")
        empty = self.write_seeds("empty.txt", "# nakagawa-miss-seeds-v1\n")
        seeded, _ = self.build("empty_seeded", f"--miss-seeds={empty}")
        for name in ("m_recomp.c", "m_recomp_0.c", "m_recomp_1.c", "m_recomp_funcs.h"):
            self.assertEqual((plain / name).read_bytes(), (seeded / name).read_bytes(), name)
        self.assertNotIn("CODEGEN_MISS_SEEDS", plain_result.stdout)
        report = json.loads((plain / "m_recomp_coverage.json").read_text(encoding="ascii"))
        self.assertNotIn("miss_seeds", report)

    def test_a_seed_adds_one_translated_function_and_is_listed_in_the_report(self):
        _plain, plain_result = self.build("count_plain")
        seeds = self.write_seeds(
            "one.txt", "# nakagawa-miss-seeds-v1\n0x00010028 run=r\n0x00010022 run=r\n")
        seeded, result = self.build("count_seeded", f"--miss-seeds={seeds}")
        self.assertEqual(self.functions_line(result), self.functions_line(plain_result) + 1)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=2 applied=1 known=0 ignored=1", result.stdout)
        self.assertIn("ignored 0x00010022: unaligned", result.stderr)
        self.assertTrue((seeded / "m_recomp_2.c").is_file())
        self.assertIn("void f_00010028(CpuState *s)",
                      "".join((seeded / f"m_recomp_{i}.c").read_text(encoding="ascii")
                              for i in range(3)))
        report = json.loads((seeded / "m_recomp_coverage.json").read_text(encoding="ascii"))
        self.assertEqual(report["miss_seeds"],
                         {"0x00010022": "unaligned", "0x00010028": "applied"})

    def test_a_seed_the_emitter_cannot_translate_is_reported_and_left_unregistered(self):
        for name, words in (("reserved", RESERVED_WORDS),
                            ("unexecuted", UNEXECUTED_RESERVED_WORDS)):
            with self.subTest(name):
                elf = self.work / f"{name}.elf"
                elf.write_bytes(_synthetic_elf(words, ENTRY))
                seeds = self.write_seeds(f"{name}.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
                out = self.work / f"{name}_out"
                out.mkdir()
                result = _codegen(elf, out / "m_recomp.c", f"--miss-seeds={seeds}")
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1",
                              result.stdout)
                self.assertIn("ignored 0x00010028: untranslatable", result.stderr)
                generated = "".join(path.read_text(encoding="ascii")
                                    for path in out.glob("m_recomp*.c"))
                self.assertNotIn("f_00010028", generated)
                report = json.loads((out / "m_recomp_coverage.json").read_text(encoding="ascii"))
                self.assertTrue(report["miss_seeds"]["0x00010028"].startswith("untranslatable"))

    def test_a_seed_on_a_gap_filled_entry_is_counted_as_known(self):
        plain, _ = self.build("gap_plain")
        seeds = self.write_seeds("gap.txt", f"# nakagawa-miss-seeds-v1\n0x{PADDING_FUNCTION:08x}\n")
        seeded, result = self.build("gap_seeded", f"--miss-seeds={seeds}")
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=1 ignored=0", result.stdout)
        self.assertEqual((plain / "m_recomp_funcs.h").read_bytes(),
                         (seeded / "m_recomp_funcs.h").read_bytes())

    def test_a_seed_left_to_the_dispatcher_is_not_applied(self):
        seeds = self.write_seeds("omitted.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        out, result = self.build("omitted", f"--miss-seeds={seeds}", "--omit-aot=10028")
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", result.stdout)
        self.assertIn("ignored 0x00010028: not-emitted", result.stderr)
        report = json.loads((out / "m_recomp_coverage.json").read_text(encoding="ascii"))
        self.assertEqual(report["miss_seeds"], {"0x00010028": "not-emitted"})

    def test_a_word_the_image_address_space_refuses_is_checked_before_applying(self):
        elf = self.work / "import_site.elf"
        elf.write_bytes(_synthetic_elf(IMPORT_SITE_WORDS, ENTRY))
        seeds = self.write_seeds("import_site.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        out = self.work / "import_site_out"
        out.mkdir()
        result = _with_import_site("codegen", str(elf), str(out / "m_recomp.c"),
                                   "--funcs-per-chunk=1", f"--miss-seeds={seeds}")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", result.stdout)
        self.assertIn(f"ignored 0x00010028: untranslatable: variable-import word32 reference "
                      f"on an executed word at 0x{IMPORT_SITE:08x}", result.stderr)
        self.assertNotIn("skip 0x00010028", result.stderr)
        generated = "".join(path.read_text(encoding="ascii") for path in out.glob("m_recomp*.c"))
        self.assertNotIn("f_00010028", generated)

    def test_a_seed_that_brings_a_stub_is_named_and_the_stub_is_not_built(self):
        elf = self.work / "brings.elf"
        elf.write_bytes(_synthetic_elf(CALLEE_STUB_WORDS, ENTRY))
        seeds = self.write_seeds("brings.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        out = self.work / "brings_out"
        out.mkdir()
        plain_out = self.work / "brings_plain"
        plain_out.mkdir()
        plain = _codegen(elf, plain_out / "m_recomp.c")
        result = _codegen(elf, out / "m_recomp.c", f"--miss-seeds={seeds}")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", result.stdout)
        self.assertIn(f"ignored 0x00010028: untranslatable: brings 0x{CALLEE:08x}: opcode 0x1d",
                      result.stderr)
        # The image has stubs of its own on the data around the seed; the seed adds none.
        self.assertEqual(self.stubs(result), self.stubs(plain))
        generated = "".join(path.read_text(encoding="ascii") for path in out.glob("m_recomp*.c"))
        self.assertNotIn(f"f_{CALLEE:08x}", generated)

    def test_a_missing_seed_list_stops_the_build(self):
        result = _codegen(self.elf_path, self.work / "gone" / "m_recomp.c",
                          f"--miss-seeds={self.work / 'absent.txt'}")
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot read the seed file", result.stderr)


def _native_toolchain():
    compiler = shutil.which("gcc") or shutil.which("cc")
    return compiler if compiler and os.environ.get("NK_SKIP_NATIVE_FIXTURES") != "1" else None


@unittest.skipUnless(_native_toolchain(), "needs a C compiler")
class EndToEndTests(_Fixture):
    """Miss, seed file, second build, no miss: the loop of #1848 with the real runtime."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.cc = _native_toolchain()
        cls.image = cls.work / "image.bin"
        cls.image.write_bytes(_image_bytes(WORDS))
        cls.config = cls.work / "title-config"
        cls.config.mkdir()
        subprocess.run(
            [sys.executable, str(ROOT / "tools" / "title_runtime_config.py"),
             "--output", str(cls.config / "sr_title_config.h")],
            cwd=ROOT, check=True, capture_output=True)
        cls.harness = cls.work / "harness.c"
        cls.harness.write_text(HARNESS, encoding="ascii", newline="\n")

    def build_and_run(self, name, *codegen_args, words=None):
        elf_path, image = self.elf_path, self.image
        if words is not None:
            elf_path, image = self.work / f"{name}.elf", self.work / f"{name}.bin"
            elf_path.write_bytes(_synthetic_elf(words, ENTRY))
            image.write_bytes(_image_bytes(words))
        out = self.work / name
        out.mkdir()
        generated = _codegen(elf_path, out / "m_recomp.c", *codegen_args)
        self.assertEqual(generated.returncode, 0, generated.stderr + generated.stdout)
        sources = [str(self.harness), *sorted(map(str, out.glob("m_recomp*.c"))),
                   *(str(ROOT / "src" / "rt" / name) for name in RUNTIME_SOURCES)]
        exe = out / "run.exe"
        build = subprocess.run(
            [self.cc, "-O0", "-fno-strict-aliasing", f"-I{ROOT / 'src' / 'rt'}",
             f"-I{ROOT / 'src' / 'core'}", f"-I{out}", f"-I{self.config}",
             "-DSR_FLIGHT_RECORDER_LINKED", "-DPERF_AOT_INSTRUCTIONS=0", "-DSR_PUBLIC_SAFE",
             "-o", str(exe), *sources, "-lm"],
            cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        miss_file = out / "miss.txt"
        env = dict(os.environ, SR_MISS_SEEDS=str(miss_file))
        run = subprocess.run(
            [str(exe), str(image), f"{ENTRY:x}", f"{ENTRY:x}", f"{TARGET:x}"],
            cwd=out, capture_output=True, text=True, env=env)
        share = next((line for line in run.stderr.splitlines()
                      if line.startswith("EXEC_SHARE ")), None)
        self.assertIsNotNone(share, f"the run stopped: {run.stderr[-1000:]}")
        fields = dict(part.split("=") for part in share.split()[1:])
        return run, fields, miss_file, generated

    def test_a_miss_is_recorded_fed_back_and_translated_next_build(self):
        # Build 1: no seeds. The computed call misses and the interpreter floor finishes it.
        first, first_fields, first_miss, first_gen = self.build_and_run("first")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("RESULT v0=42", first.stdout)
        self.assertIn(f"DISPATCH_MISS_NEW[1]: target=0x{TARGET:08x}", first.stderr)
        self.assertEqual(first_fields["miss_targets"], "1")
        self.assertEqual(first_fields["entries_dispatch_miss"], "1")
        recorded = miss_seeds.read_seed_file(first_miss)
        self.assertEqual(set(recorded.seeds), {TARGET})
        self.assertEqual(recorded.overflow, 0)

        # The tool merges the run's file into the title's seed list.
        seed_list = self.work / "title_seeds.txt"
        merged = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "miss_seeds.py"), "merge", f"--out={seed_list}",
             "--run=fixture-1", f"--elf={self.elf_path}", str(first_miss)],
            cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(merged.returncode, 0, merged.stderr)

        # Build 2: the seed makes the target a translated function. The run does not miss.
        second, second_fields, second_miss, second_gen = self.build_and_run(
            "second", f"--miss-seeds={seed_list}")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("RESULT v0=42", second.stdout)
        self.assertNotIn("DISPATCH_MISS_NEW", second.stderr)
        self.assertEqual(second_fields["miss_targets"], "0")
        self.assertEqual(second_fields["interp_entries"], "0")
        self.assertEqual(set(miss_seeds.read_seed_file(second_miss).seeds), set())

        before = CodegenSeedTests.functions_line(first_gen)
        after = CodegenSeedTests.functions_line(second_gen)
        self.assertEqual(after, before + 1)

    def test_a_seed_never_makes_a_miss_permissive(self):
        # The seed names an address no function can start at; the build ignores it, and the run
        # still misses on the real target exactly as before.
        bad = self.write_seeds("unusable.txt", "# nakagawa-miss-seeds-v1\n0x00020000\n")
        run, fields, miss_file, generated = self.build_and_run("unusable", f"--miss-seeds={bad}")
        self.assertIn("ignored 0x00020000: outside-executable-ranges", generated.stderr)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f"DISPATCH_MISS_NEW[1]: target=0x{TARGET:08x}", run.stderr)
        self.assertEqual(fields["miss_targets"], "1")
        self.assertEqual(set(miss_seeds.read_seed_file(miss_file).seeds), {TARGET})

    def test_a_seed_on_an_entry_codegen_would_stub_leaves_the_run_unchanged(self):
        # The first word is valid and the run returns normally, but the walk covers a reserved
        # word the run never executes. Registering the entry would stub it and abort the run on
        # entry; the seed is ignored instead and the miss keeps reaching the interpreter floor.
        seeds = self.write_seeds("stub.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        run, fields, miss_file, generated = self.build_and_run(
            "unexecuted", f"--miss-seeds={seeds}", words=UNEXECUTED_RESERVED_WORDS)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", generated.stdout)
        self.assertIn("ignored 0x00010028: untranslatable", generated.stderr)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("RESULT v0=42", run.stdout)
        self.assertNotIn("sr_unimplemented", run.stderr)
        self.assertEqual(fields["miss_targets"], "1")
        self.assertEqual(set(miss_seeds.read_seed_file(miss_file).seeds), {TARGET})

    def test_a_seed_that_brings_a_stub_leaves_the_run_unchanged(self):
        # The seed's own entry translates; the entry its `jal` adds would not, and a run reaches
        # that entry. The seed is refused, so the miss keeps reaching the interpreter floor.
        seeds = self.write_seeds("brings_run.txt", "# nakagawa-miss-seeds-v1\n0x00010028\n")
        run, fields, miss_file, generated = self.build_and_run(
            "brings", f"--miss-seeds={seeds}", words=CALLEE_STUB_WORDS)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", generated.stdout)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("RESULT v0=42", run.stdout)
        self.assertNotIn("sr_unimplemented", run.stderr)
        self.assertEqual(fields["miss_targets"], "1")
        self.assertEqual(set(miss_seeds.read_seed_file(miss_file).seeds), {TARGET})

    def test_a_seed_inside_a_live_frame_leaves_the_run_unchanged(self):
        # MID_FRAME has a valid body, but as a callable entry that body restores the $sp it was
        # entered with, which undoes its own epilogue. The floor hands off to any address with a
        # body, so a body here would stop a run the floor finishes.
        seeds = self.write_seeds("mid_frame.txt", f"# nakagawa-miss-seeds-v1\n0x{MID_FRAME:08x}\n")
        run, fields, miss_file, generated = self.build_and_run(
            "mid_frame", f"--miss-seeds={seeds}", words=MID_FRAME_WORDS)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", generated.stdout)
        self.assertIn(f"ignored 0x{MID_FRAME:08x}: not-callable: continuation", generated.stderr)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("RESULT v0=42", run.stdout)
        self.assertNotIn("SEMANTIC_BOUNDARY", run.stderr)
        self.assertEqual(fields["miss_targets"], "1")
        self.assertEqual(set(miss_seeds.read_seed_file(miss_file).seeds), {TARGET})

    def test_the_loop_leaves_a_longjmp_continuation_on_the_floor(self):
        # The floor runs the continuation by design: nothing misses, yet the run's file lists it
        # and `merge` keeps it. The build refuses it, and the second run ends as the first did.
        first, _fields, first_miss, _gen = self.build_and_run("longjmp_1", words=LONGJMP_WORDS)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("RESULT v0=42", first.stdout)
        self.assertNotIn("DISPATCH_MISS_NEW", first.stderr)
        seed_list = self.work / "longjmp_seeds.txt"
        merged = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "miss_seeds.py"), "merge", f"--out={seed_list}",
             "--run=fixture-1", f"--elf={self.work / 'longjmp_1.elf'}", str(first_miss)],
            cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(merged.returncode, 0, merged.stderr)
        self.assertEqual(set(miss_seeds.read_seed_file(seed_list).seeds), {CONTINUATION})

        second, _fields, _miss, generated = self.build_and_run(
            "longjmp_2", f"--miss-seeds={seed_list}", words=LONGJMP_WORDS)
        self.assertIn("CODEGEN_MISS_SEEDS seeds=1 applied=0 known=0 ignored=1", generated.stdout)
        self.assertIn(f"ignored 0x{CONTINUATION:08x}: not-callable: continuation", generated.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("RESULT v0=42", second.stdout)
        self.assertNotIn("SEMANTIC_BOUNDARY", second.stderr)


if __name__ == "__main__":
    unittest.main()
