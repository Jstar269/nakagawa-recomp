# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""End-to-end regressions for callable versus resume-entry codegen semantics.

The fixture is an owned, synthetic ELF32/MIPS executable.  Its callable owner
allocates a 0x70-byte frame and then reaches an interior entry which restores
that frame and returns.  Entering the interior PC directly therefore models a
real indirect resume with the owner's frame already active.
"""

from __future__ import annotations

from io import StringIO
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import unittest.mock


ROOT = Path(__file__).resolve().parent.parent
CODEGEN = ROOT / "tools" / "codegen.py"
CC = shutil.which("gcc")
sys.path.insert(0, str(ROOT / "tools"))
import analyze  # noqa: E402
import codegen  # noqa: E402
import entry_frame_balance  # noqa: E402
import scratch_dir  # noqa: E402

OWNER = 0x00021AC0
RESUME = 0x00021C78
ADJACENT = 0x00021C88
TINY_LEAF = 0x00021C98
INDIRECT_CALLER = 0x00021CA0
TAIL_SOURCE = 0x00021CC0
TAIL_TARGET = 0x00021CD0
CALLER_SP = 0x00001000
FRAME_SIZE = 0x70


# The #855 TAIL transfer protocol, as the stand-in recomp.h below declares it and the
# stand-in harnesses implement it: the same slot, drain and first-restore-wins rule as
# src/rt/recomp.h and src/rt/guest_interp.c, with the stand-in dispatch() running a
# computed target to completion. The header also stands in for the entry host-stack check
# of bodies that make direct calls (#855): these harnesses run no scheduler or guest fiber,
# where the production check has nothing to bound either.
_TAIL_PROTOCOL_HEADER = """\
typedef void (*TestBodyFn)(CpuState *);
int sr_aot_return_boundary_begin(CpuState *s, uint32_t resume_pc,
                                 uint32_t call_pc, uint32_t has_resume_pc);
int sr_aot_return_boundary_end(CpuState *s);
#define SR_AOT_RETURN_CAPTURE(s, target) ((void)(s), (void)(target))
void sr_tail_record(CpuState *s, uint32_t target, TestBodyFn fn,
                    uint32_t restore_sp, uint32_t owes_restore);
void sr_tail_drain(CpuState *s);
#define SR_TAIL_DRAIN(s) sr_tail_drain(s)
#define SR_HOST_STACK_CHECK(entry) ((void)(entry))
"""
_TAIL_PROTOCOL_IMPL = """\
int sr_aot_return_boundary_begin(CpuState *s, uint32_t resume_pc,
                                 uint32_t call_pc, uint32_t has_resume_pc) {
    (void)s; (void)resume_pc; (void)call_pc; (void)has_resume_pc; return 0;
}
int sr_aot_return_boundary_end(CpuState *s) {
    (void)s; return 0;
}
static struct {
    TestBodyFn fn; CpuState *state; uint32_t target, restore_sp; int pending, owes;
} test_tail;
void sr_tail_record(CpuState *s, uint32_t target, TestBodyFn fn,
                    uint32_t restore_sp, uint32_t owes_restore) {
    if (test_tail.pending) abort();
    test_tail.fn = fn; test_tail.state = s; test_tail.target = target;
    test_tail.restore_sp = restore_sp; test_tail.owes = owes_restore != 0u;
    test_tail.pending = 1;
}
void sr_tail_drain(CpuState *s) {
    uint32_t chain_sp = 0u;
    int restores = 0;
    while (test_tail.pending) {
        TestBodyFn fn = test_tail.fn;
        uint32_t target = test_tail.target;
        if (test_tail.state != s) abort();
        if (test_tail.owes && !restores) { chain_sp = test_tail.restore_sp; restores = 1; }
        test_tail.pending = 0;
        if (fn) fn(s); else dispatch(s, target);
    }
    if (restores) s->r[29] = chain_sp;
}
"""


def _synthetic_elf(
    words: dict[int, int],
    entry: int,
    pointers=(),
    e_type: int = 2,
    reloc_section: int | None = None,
) -> bytes:
    """Build a minimal ELF32/MIPS with executable .text and pointer-bearing .data.

    `e_type` selects the ELF class (default ET_EXEC). `reloc_section`, when
    given, adds an empty relocation section of that section type (SHT_REL=9
    for the -Wl,-q PSPDEV form, or SHT_PSP_RELA=0x700000a0), which is the
    signal that the input still needs rebasing at a nonzero base.
    """
    lo = min(words)
    hi = max(words) + 4
    text_size = hi - lo
    data_addr = (hi + 0xFF) & ~0xFF
    segment = bytearray(data_addr - lo + 4 * len(pointers))
    for addr, word in words.items():
        struct.pack_into("<I", segment, addr - lo, word & 0xFFFFFFFF)
    for index, pointer in enumerate(pointers):
        struct.pack_into("<I", segment, data_addr - lo + index * 4, pointer)

    data_off = 0x100
    shstr = b"\0.text\0.data\0.shstrtab\0"
    shstr_off = data_off + len(segment)
    shoff = (shstr_off + len(shstr) + 3) & ~3
    section_count = 4 if reloc_section is None else 5
    total = shoff + section_count * 40
    blob = bytearray(total)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    blob[:52] = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        e_type, 8, 1, entry, 52, shoff, 0x50001000,
        52, 32, 1, 40, section_count, 3,
    )
    blob[52:84] = struct.pack(
        "<8I", 1, data_off, lo, lo, len(segment), len(segment), 7, 0x1000
    )
    blob[data_off:data_off + len(segment)] = segment
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, lo, data_off, text_size, 0, 0, 4, 0),
        (7, 1, 3, data_addr, data_off + data_addr - lo, 4 * len(pointers), 0, 0, 4, 0),
        (13, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),
    ]
    if reloc_section is not None:
        sections.append((0, reloc_section, 8, 0, data_off, 0, 0, 0, 0, 0))
    for index, section in enumerate(sections):
        struct.pack_into("<10I", blob, shoff + index * 40, *section)
    return bytes(blob)


def _fixture_words() -> dict[int, int]:
    words = {
        OWNER: 0x27BDFF90,       # addiu sp, sp, -0x70
        OWNER + 4: 0xAFBF006C,   # sw ra, 0x6c(sp)
        OWNER + 8: 0x0C000000 | ((ADJACENT >> 2) & 0x03FFFFFF), # jal adjacent
        OWNER + 12: 0x00000000,  # nop
        RESUME: 0x8FBF006C,      # lw ra, 0x6c(sp)
        RESUME + 4: 0x27BD0070,  # addiu sp, sp, 0x70
        RESUME + 8: 0x03E00008,  # jr ra
        RESUME + 12: 0x00000000, # nop
        ADJACENT: 0x27BDFFF0,       # addiu sp, sp, -0x10
        ADJACENT + 4: 0x24020007,   # addiu v0, zero, 7
        ADJACENT + 8: 0x03E00008,   # jr ra
        ADJACENT + 12: 0x27BD0010,  # addiu sp, sp, 0x10
        TINY_LEAF: 0x03E00008,      # jr ra
        TINY_LEAF + 4: 0x00801021,  # addu v0, a0, zero
        INDIRECT_CALLER: 0x27BDFFF0,
        INDIRECT_CALLER + 4: 0xAFBF000C,
        INDIRECT_CALLER + 8: 0x3C190002,  # lui t9, 2
        INDIRECT_CALLER + 12: 0x37391C98, # ori t9, t9, 0x1c98
        INDIRECT_CALLER + 16: 0x0320F809, # jalr ra, t9
        INDIRECT_CALLER + 20: 0x24040009, # addiu a0, zero, 9
        INDIRECT_CALLER + 24: 0x8FBF000C,
        INDIRECT_CALLER + 28: 0x03E00008,
        INDIRECT_CALLER + 32: 0x27BD0010,
        TAIL_SOURCE: 0x3C190002,       # lui t9, 2
        TAIL_SOURCE + 4: 0x37391CD0,   # ori t9, t9, 0x1cd0
        TAIL_SOURCE + 8: 0x03200008,   # jr t9 (non-linking tail transfer)
        TAIL_SOURCE + 12: 0x00000000,
        TAIL_TARGET: 0x2402000B,       # addiu v0, zero, 11
        TAIL_TARGET + 4: 0x03E00008,
        TAIL_TARGET + 8: 0x00000000,
    }
    # Owned padding is deliberately executable so the owner's natural path
    # reaches the same resume region without introducing another boundary.
    for addr in range(OWNER + 16, RESUME, 4):
        words[addr] = 0
    return words


@unittest.skipUnless(CC, "gcc is required for the generated-C execution regression")
class EntrySemanticsPipelineTests(unittest.TestCase):
    def test_indirect_resume_preserves_guest_epilogue_sp(self):
        assert CC is not None
        with scratch_dir.TemporaryDirectory(prefix="entry_semantics_") as tmp:
            work = Path(tmp)
            elf = work / "entry_semantics.elf"
            generated = work / "entry_semantics.c"
            elf.write_bytes(_synthetic_elf(
                _fixture_words(), OWNER,
                pointers=(TINY_LEAF, INDIRECT_CALLER, TAIL_SOURCE, TAIL_TARGET),
            ))

            env = dict(os.environ)
            result = subprocess.run(
                [sys.executable, str(CODEGEN), str(elf), str(generated), "--profile=hst"],
                cwd=ROOT, env=env, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            generated_chunks = "\n".join(
                path.read_text(encoding="ascii")
                for path in sorted(work.glob("entry_semantics_*.c"))
            )
            owner_start = generated_chunks.index(f"void f_{OWNER:08x}")
            owner_end = generated_chunks.index("\nvoid ", owner_start + 1)
            owner_body = generated_chunks[owner_start:owner_end]
            self.assertIn(f"0x{RESUME:08x}u", owner_body)
            self.assertNotIn(f"r_{RESUME:08x}(s)", owner_body)

            resume_start = generated_chunks.index(f"void r_{RESUME:08x}")
            resume_end = generated_chunks.index("\nvoid ", resume_start + 1)
            resume_body = generated_chunks[resume_start:resume_end]
            self.assertNotIn("_sp_entry", resume_body)
            self.assertIn(
                f"sr_register(0x{RESUME:08x}u, r_{RESUME:08x});",
                generated_chunks,
            )

            (work / "recomp.h").write_text(
                """\
#ifndef TEST_RECOMP_H
#define TEST_RECOMP_H
#include <stdint.h>
#define SR_CPUSTATE_ABI_VERSION 3u
typedef struct CpuState { uint32_t r[32]; uint32_t pc; } CpuState;
uint32_t test_mem_r32(uint32_t addr);
void test_mem_w32(uint32_t addr, uint32_t value);
#define MEM_R32(a) test_mem_r32((uint32_t)(a))
#define MEM_W32(a, v) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define MEM_W32_PC(a, v, pc) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define SR_YIELD(s, pc) ((void)0)
#define sr_begin(s, pc, op) ((void)0)
#define sr_end(s, addr, size) ((void)0)
void dispatch(CpuState *s, uint32_t target);
void dispatch_call(CpuState *s, uint32_t target, uint32_t resume_pc);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
""" + _TAIL_PROTOCOL_HEADER + """#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "entry_semantics_funcs.h"
{_TAIL_PROTOCOL_IMPL}
static uint32_t mem[0x2000 / 4];
uint32_t test_mem_r32(uint32_t addr) {{ return mem[(addr & 0x1fffu) / 4u]; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ mem[(addr & 0x1fffu) / 4u] = value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_exec_span_reset(void) {{}}
int sr_exec_span_register(uint32_t start, uint32_t end) {{ (void)start; (void)end; return 1; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
void dispatch(CpuState *s, uint32_t target) {{
    if (target == 0x{RESUME:08x}u) {{ r_{RESUME:08x}(s); sr_tail_drain(s); return; }}
    if (target == 0x{TINY_LEAF:08x}u) {{ f_{TINY_LEAF:08x}(s); sr_tail_drain(s); return; }}
    if (target == 0x{TAIL_TARGET:08x}u) {{ f_{TAIL_TARGET:08x}(s); sr_tail_drain(s); return; }}
    abort();
}}
void dispatch_call(CpuState *s, uint32_t target, uint32_t resume_pc) {{
    (void)resume_pc;
    dispatch(s, target);
}}

int main(void) {{
    CpuState resumed = {{0}};
    resumed.r[29] = 0x{CALLER_SP - FRAME_SIZE:08x}u;
    test_mem_w32(0x{CALLER_SP - 4:08x}u, 0x12345678u);
    r_{RESUME:08x}(&resumed); sr_tail_drain(&resumed);

    CpuState called = {{0}};
    called.r[29] = 0x{CALLER_SP:08x}u;
    called.r[31] = 0x12345678u;
    f_{OWNER:08x}(&called); sr_tail_drain(&called);

    CpuState adjacent = {{0}};
    adjacent.r[29] = 0x{CALLER_SP:08x}u;
    f_{ADJACENT:08x}(&adjacent); sr_tail_drain(&adjacent);

    CpuState leaf = {{0}};
    leaf.r[4] = 5u;
    leaf.r[29] = 0x{CALLER_SP:08x}u;
    f_{TINY_LEAF:08x}(&leaf); sr_tail_drain(&leaf);

    CpuState indirect = {{0}};
    indirect.r[29] = 0x{CALLER_SP:08x}u;
    indirect.r[31] = 0x12345678u;
    f_{INDIRECT_CALLER:08x}(&indirect); sr_tail_drain(&indirect);

    CpuState tail = {{0}};
    tail.r[29] = 0x{CALLER_SP:08x}u;
    tail.r[31] = 0x12345678u;
    f_{TAIL_SOURCE:08x}(&tail); sr_tail_drain(&tail);

    printf("resume_sp=0x%08x owner_sp=0x%08x indirect_v0=%u tail_v0=%u\\n",
           resumed.r[29], called.r[29], indirect.r[2], tail.r[2]);
    return resumed.r[29] == 0x{CALLER_SP:08x}u
        && called.r[29] == 0x{CALLER_SP:08x}u
        && adjacent.r[29] == 0x{CALLER_SP:08x}u && adjacent.r[2] == 7u
        && leaf.r[29] == 0x{CALLER_SP:08x}u && leaf.r[2] == 5u
        && indirect.r[29] == 0x{CALLER_SP:08x}u && indirect.r[2] == 9u
        && tail.r[29] == 0x{CALLER_SP:08x}u && tail.r[2] == 11u ? 0 : 1;
}}
""",
                encoding="ascii",
            )
            chunks = sorted(work.glob("entry_semantics_*.c"))
            exe = work / "entry_semantics.exe"
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", "-I", str(work), str(work / "harness.c"),
                 str(generated), *(str(path) for path in chunks), "-lm", "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
            self.assertIn(
                "resume_sp=0x00001000 owner_sp=0x00001000 indirect_v0=9 tail_v0=11",
                ran.stdout,
            )

    def test_direct_j_resume_dispatch_preserves_live_guest_sp(self):
        """The generalized direct-j rule reaches the real resume host entry.

        The owner jumps directly to its shared epilogue.  The analyzer must
        retain that target as an ``r_`` entry (not an ``f_`` boundary), and an
        indirect dispatch into it must let the guest epilogue restore the live
        frame rather than applying a synthetic entry-SP restore.
        """
        assert CC is not None
        owner = 0x00001000
        resume = 0x00001010
        caller_sp = 0x00001000
        frame = 0x20
        words = {
            owner: 0x27BDFFE0,             # addiu sp, sp, -0x20
            owner + 4: 0xAFAF001C,         # sw ra, 0x1c(sp)
            owner + 8: 0x08000404,         # j 0x1010
            owner + 12: 0x00000000,        # delay slot
            resume: 0x8FAF001C,            # lw ra, 0x1c(sp)
            resume + 4: 0x27BD0020,        # addiu sp, sp, 0x20
            resume + 8: 0x03E00008,        # jr ra
            resume + 12: 0x00000000,
        }
        with scratch_dir.TemporaryDirectory(prefix="direct_j_semantics_") as tmp:
            work = Path(tmp)
            elf = work / "direct_j_semantics.elf"
            generated = work / "direct_j_semantics.c"
            elf.write_bytes(_synthetic_elf(words, owner))
            env = dict(os.environ)
            result = subprocess.run(
                [sys.executable, str(CODEGEN), str(elf), str(generated), "--profile=hst"],
                cwd=ROOT, env=env, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            chunks = "\n".join(
                path.read_text(encoding="ascii")
                for path in sorted(work.glob("direct_j_semantics_*.c"))
            )
            self.assertIn(f"void r_{resume:08x}(CpuState *s)", chunks)
            self.assertNotIn(f"void f_{resume:08x}(CpuState *s)", chunks)
            self.assertIn(f"L_{resume:08x}:", chunks)
            self.assertNotIn(f"r_{resume:08x}(s);", chunks)
            resume_start = chunks.index(f"void r_{resume:08x}")
            resume_end = chunks.index("\nvoid ", resume_start + 1)
            self.assertNotIn("_sp_entry", chunks[resume_start:resume_end])
            self.assertIn(
                f"sr_register(0x{resume:08x}u, r_{resume:08x});", chunks
            )

            (work / "recomp.h").write_text(
                """\
#ifndef TEST_RECOMP_H
#define TEST_RECOMP_H
#include <stdint.h>
#define SR_CPUSTATE_ABI_VERSION 3u
typedef struct CpuState { uint32_t r[32]; uint32_t pc; } CpuState;
uint32_t test_mem_r32(uint32_t addr);
void test_mem_w32(uint32_t addr, uint32_t value);
#define MEM_R32(a) test_mem_r32((uint32_t)(a))
#define MEM_W32(a, v) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define MEM_W32_PC(a, v, pc) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define SR_YIELD(s, pc) ((void)0)
#define sr_begin(s, pc, op) ((void)0)
#define sr_end(s, addr, size) ((void)0)
void dispatch(CpuState *s, uint32_t target);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
""" + _TAIL_PROTOCOL_HEADER + """#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdlib.h>
#include "direct_j_semantics_funcs.h"
{_TAIL_PROTOCOL_IMPL}static uint32_t mem[0x2000 / 4];
uint32_t test_mem_r32(uint32_t addr) {{ return mem[(addr & 0x1fffu) / 4u]; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ mem[(addr & 0x1fffu) / 4u] = value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_exec_span_reset(void) {{}}
int sr_exec_span_register(uint32_t start, uint32_t end) {{ (void)start; (void)end; return 1; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
void dispatch(CpuState *s, uint32_t target) {{
    if (target == 0x{resume:08x}u) {{ r_{resume:08x}(s); sr_tail_drain(s); return; }}
    abort();
}}
int main(void) {{
    CpuState state = {{0}};
    state.r[29] = 0x{caller_sp - frame:08x}u;
    test_mem_w32(0x{caller_sp - 4:08x}u, 0x12345678u);
    dispatch(&state, 0x{resume:08x}u);
    return state.r[29] == 0x{caller_sp:08x}u ? 0 : 1;
}}
""",
                encoding="ascii",
            )
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", "-I", str(work), str(work / "harness.c"),
                 str(generated), *(str(path) for path in sorted(work.glob("direct_j_semantics_*.c"))),
                 "-lm", "-o", str(work / "direct_j_semantics.exe")],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
            ran = subprocess.run([str(work / "direct_j_semantics.exe")], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)

    def test_direct_j_to_known_entry_is_tail_after_this_adjustment_slot(self):
        owner = 0x00001000
        target = 0x00001010
        words = {
            owner: (0x02 << 26) | ((target >> 2) & 0x03FFFFFF),  # j target
            owner + 4: 0x2484FFE0,                              # addiu a0,a0,-32
            owner + 8: 0x03E00008,                              # unreachable return
            owner + 12: 0x00000000,
            target: 0x00801021,                                 # addu v0,a0,zero
            target + 4: 0x03E00008,
            target + 8: 0x00000000,
        }
        with scratch_dir.TemporaryDirectory(prefix="known_j_tail_") as tmp:
            path = Path(tmp) / "known_j_tail.elf"
            path.write_bytes(_synthetic_elf(words, owner))
            elf = analyze.Elf(os.fspath(path))
            starts, ranges = analyze.analyze(elf)
            self.assertIn(target, starts, "the direct-j callee must be an analyzer-owned entry")
            body = "\n".join(codegen.emit_function(elf, owner, ranges, starts))
            slot = body.find("s->r[4] =")
            tail = body.find(f"sr_tail_record(s, 0x{target:08x}u,")
            self.assertGreaterEqual(slot, 0, body)
            self.assertGreaterEqual(tail, 0, body)
            self.assertLess(slot, tail, "the this-adjustment delay slot must run before the TAIL record")
            self.assertNotIn(f"goto L_{target:08x}", body,
                             "a known function entry must not be treated as an intra-function label")

    def test_variadic_argument_save_stub_is_an_ordinary_analyzed_function(self):
        owner = 0x00001000
        words = {owner: 0x27BDFFE0}  # addiu sp,sp,-32
        for index, register in enumerate(range(4, 12)):
            words[owner + 4 + 4 * index] = (
                (0x2B << 26) | (29 << 21) | (register << 16) | (index * 4)
            )
        words[owner + 36] = 0x03E00008               # jr ra
        words[owner + 40] = 0x27BD0020               # addiu sp,sp,32

        with scratch_dir.TemporaryDirectory(prefix="variadic_save_entry_") as tmp:
            path = Path(tmp) / "variadic_save.elf"
            path.write_bytes(_synthetic_elf(words, owner))
            elf = analyze.Elf(os.fspath(path))
            starts, ranges = analyze.analyze(elf)
            self.assertIn(owner, starts)
            self.assertTrue(analyze.in_ranges(owner, ranges))
            body = "\n".join(codegen.emit_function(elf, owner, ranges, starts))
            self.assertEqual(body.count("MEM_W32_PC("), 8, body)
            for offset in range(0, 32, 4):
                self.assertIn(f"s->r[29] + 0x{offset:08x}u", body)

    def test_omitted_direct_j_target_reaches_runtime_dispatch(self):
        """An omitted direct-j target must not be absorbed into its AOT caller.

        The dead ``jal`` is only a discovery anchor: it makes the target a valid
        ``--omit-aot`` address without making it reachable from the tested owner.
        The owner itself uses a direct tail ``j``.  The generated caller is compiled
        and executed against a small dispatch seam, so a source-shape-only omission
        cannot pass if the target's body is silently inlined.
        """
        assert CC is not None
        owner = 0x00001000
        anchor = 0x00001008
        target = 0x00001040
        with scratch_dir.TemporaryDirectory(prefix="direct_j_omission_") as tmp:
            work = Path(tmp)
            elf = work / "direct_j_omission.elf"
            generated = work / "direct_j_omission.c"
            words = {
                owner: 0x08000410,       # j target
                owner + 4: 0x24030011,   # delay slot: v1 = 0x11
                anchor: 0x0C000410,      # dead jal target: discovery anchor only
                anchor + 4: 0x24000000,
                target: 0x24020022,      # addiu v0, zero, 0x22
                target + 4: 0x03E00008,  # jr ra
                target + 8: 0x24630044,  # return delay: v1 += 0x44
            }
            elf.write_bytes(_synthetic_elf(words, owner))
            env = dict(os.environ)
            result = subprocess.run(
                [sys.executable, str(CODEGEN), str(elf), str(generated),
                 "--profile=hst", "--funcs-per-chunk=1",
                 f"--omit-aot=0x{target:08x}"],
                cwd=ROOT, env=env, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            generated_text = generated.read_text(encoding="ascii") + "\n".join(
                path.read_text(encoding="ascii")
                for path in sorted(work.glob("direct_j_omission_[0-9]*.c"))
            )
            self.assertNotIn(f"void f_{target:08x}(CpuState *s)", generated_text)
            self.assertIn(
                f"{{ uint32_t _t = 0x{target:08x}u; sr_tail_record(s, _t, NULL,",
                generated_text,
            )

            (work / "recomp.h").write_text(
                """\
#ifndef TEST_RECOMP_H
#define TEST_RECOMP_H
#include <stdint.h>
#define SR_CPUSTATE_ABI_VERSION 3u
typedef struct CpuState { uint32_t r[32]; uint32_t pc; } CpuState;
uint32_t test_mem_r32(uint32_t addr);
void test_mem_w32(uint32_t addr, uint32_t value);
#define MEM_R32(a) test_mem_r32((uint32_t)(a))
#define MEM_W32(a, v) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define MEM_W32_PC(a, v, pc) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define SR_YIELD(s, pc) ((void)0)
#define sr_begin(s, pc, op) ((void)0)
#define sr_end(s, addr, size) ((void)0)
void dispatch(CpuState *s, uint32_t target);
void dispatch_call(CpuState *s, uint32_t target, uint32_t resume_pc);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
""" + _TAIL_PROTOCOL_HEADER + """#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "direct_j_omission_funcs.h"
{_TAIL_PROTOCOL_IMPL}
static int dispatch_hits = 0;
uint32_t test_mem_r32(uint32_t addr) {{ (void)addr; return 0u; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ (void)addr; (void)value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_exec_span_reset(void) {{}}
int sr_exec_span_register(uint32_t start, uint32_t end) {{ (void)start; (void)end; return 1; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; abort(); }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; abort(); }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; abort(); }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
static void dispatch_target(CpuState *s, uint32_t actual) {{
    if (actual != 0x{target:08x}u) abort();
    dispatch_hits++;
    s->r[2] = 0x22u;
    s->r[3] += 0x44u;
    s->pc = s->r[31];
}}
void dispatch(CpuState *s, uint32_t actual) {{ dispatch_target(s, actual); }}
void dispatch_call(CpuState *s, uint32_t actual, uint32_t resume_pc) {{
    (void)resume_pc;
    dispatch_target(s, actual);
}}

int main(void) {{
    CpuState state = {{0}};
    state.r[31] = 0x12345678u;
    f_{owner:08x}(&state); sr_tail_drain(&state);
    printf("dispatch_hits=%d v0=0x%08x v1=0x%08x\\n",
           dispatch_hits, state.r[2], state.r[3]);
    return dispatch_hits == 1 && state.r[2] == 0x22u && state.r[3] == 0x55u ? 0 : 1;
}}
""",
                encoding="ascii",
            )
            chunks = sorted(work.glob("direct_j_omission_[0-9]*.c"))
            exe = work / "direct_j_omission.exe"
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", "-I", str(work), str(work / "harness.c"),
                 str(generated), *(str(path) for path in chunks),
                 "-lm", "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
            self.assertIn("dispatch_hits=1 v0=0x00000022 v1=0x00000055", ran.stdout)


    def test_direct_branch_resume_dispatch_preserves_live_guest_sp(self):
        """The conditional-branch rule reaches the real resume host entry.

        The fixture reproduces the shape the private audit actually finds: an
        **outlined shared epilogue** -- a bare ``jr $ra`` whose delay slot
        releases the owner's frame -- sitting immediately after an unconditional
        ``b`` and entered by a forward conditional branch from inside the owner.

        That shape matters because ``analyze()`` promotes it to a *high
        confidence* function start (``b`` followed by ``jr $ra`` two slots on),
        so before this slice it was emitted as a standalone ``f_`` callable with
        the entry-SP contract, which is exactly the #51 defect.  There is no
        ``j`` anywhere in the fixture, so the direct-``j`` rule contributes
        nothing and only the branch rule can produce this result.
        """
        assert CC is not None
        owner = 0x00001000
        resume = 0x0000101C
        caller_sp = 0x00001000
        frame = 0x20
        words = {
            owner: 0x27BDFFE0,             # addiu sp, sp, -0x20
            owner + 4: 0xAFBF001C,         # sw ra, 0x1c(sp)
            owner + 8: 0x8FBF001C,         # lw ra, 0x1c(sp)
            owner + 12: 0x14800003,        # bne a0, zero, +3 -> resume (forward)
            owner + 16: 0x00000000,        # delay slot
            owner + 20: 0x10000001,        # b +1 -> resume (no fall-through)
            owner + 24: 0x00000000,        # delay slot
            resume: 0x03E00008,            # jr ra          <- the shared epilogue
            resume + 4: 0x27BD0020,        # addiu sp, sp, 0x20 (in the delay slot)
        }
        with scratch_dir.TemporaryDirectory(prefix="direct_branch_semantics_") as tmp:
            work = Path(tmp)
            elf = work / "direct_branch_semantics.elf"
            generated = work / "direct_branch_semantics.c"
            elf.write_bytes(_synthetic_elf(words, owner))
            env = dict(os.environ)
            result = subprocess.run(
                [sys.executable, str(CODEGEN), str(elf), str(generated), "--profile=hst"],
                cwd=ROOT, env=env, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            chunks = "\n".join(
                path.read_text(encoding="ascii")
                for path in sorted(work.glob("direct_branch_semantics_*.c"))
            )
            self.assertIn(f"void r_{resume:08x}(CpuState *s)", chunks)
            self.assertNotIn(f"void f_{resume:08x}(CpuState *s)", chunks)
            # The owner keeps the region native instead of calling out to it.
            self.assertIn(f"L_{resume:08x}:", chunks)
            self.assertNotIn(f"r_{resume:08x}(s);", chunks)
            resume_start = chunks.index(f"void r_{resume:08x}")
            resume_end = chunks.index("\nvoid ", resume_start + 1)
            self.assertNotIn("_sp_entry", chunks[resume_start:resume_end])
            # `resumable != unregistered`.
            self.assertIn(
                f"sr_register(0x{resume:08x}u, r_{resume:08x});", chunks
            )

            (work / "recomp.h").write_text(
                """\
#ifndef TEST_RECOMP_H
#define TEST_RECOMP_H
#include <stdint.h>
#define SR_CPUSTATE_ABI_VERSION 3u
typedef struct CpuState { uint32_t r[32]; uint32_t pc; } CpuState;
uint32_t test_mem_r32(uint32_t addr);
void test_mem_w32(uint32_t addr, uint32_t value);
#define MEM_R32(a) test_mem_r32((uint32_t)(a))
#define MEM_W32(a, v) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define MEM_W32_PC(a, v, pc) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define SR_YIELD(s, pc) ((void)0)
#define sr_begin(s, pc, op) ((void)0)
#define sr_end(s, addr, size) ((void)0)
void dispatch(CpuState *s, uint32_t target);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
""" + _TAIL_PROTOCOL_HEADER + """#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "direct_branch_semantics_funcs.h"
{_TAIL_PROTOCOL_IMPL}static uint32_t mem[0x2000 / 4];
uint32_t test_mem_r32(uint32_t addr) {{ return mem[(addr & 0x1fffu) / 4u]; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ mem[(addr & 0x1fffu) / 4u] = value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_exec_span_reset(void) {{}}
int sr_exec_span_register(uint32_t start, uint32_t end) {{ (void)start; (void)end; return 1; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
void dispatch(CpuState *s, uint32_t target) {{
    if (target == 0x{resume:08x}u) {{ r_{resume:08x}(s); sr_tail_drain(s); return; }}
    abort();
}}
int main(void) {{
    /* Resume dispatched from outside the owner, with the owner's frame live. */
    CpuState resumed = {{0}};
    resumed.r[29] = 0x{caller_sp - frame:08x}u;
    dispatch(&resumed, 0x{resume:08x}u);

    /* The owner's own native path through the same region still balances. */
    CpuState native = {{0}};
    native.r[29] = 0x{caller_sp:08x}u;
    f_{owner:08x}(&native); sr_tail_drain(&native);

    printf("resume_sp=0x%08x owner_sp=0x%08x\\n", resumed.r[29], native.r[29]);
    return resumed.r[29] == 0x{caller_sp:08x}u
        && native.r[29] == 0x{caller_sp:08x}u ? 0 : 1;
}}
""",
                encoding="ascii",
            )
            compiled = subprocess.run(
                [CC, "-std=c11", "-O0", "-I", str(work), str(work / "harness.c"),
                 str(generated),
                 *(str(path) for path in sorted(work.glob("direct_branch_semantics_*.c"))),
                 "-lm", "-o", str(work / "direct_branch_semantics.exe")],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
            ran = subprocess.run(
                [str(work / "direct_branch_semantics.exe")],
                capture_output=True, text=True,
            )
            self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
            self.assertIn(
                "resume_sp=0x00001000 owner_sp=0x00001000", ran.stdout
            )


def _minimal_elf_path(words, entry, *, e_type=2, reloc_section=None):
    path = Path(tempfile.mkdtemp(prefix="elf_routing_")) / "input.elf"
    path.write_bytes(_synthetic_elf(words, entry, e_type=e_type, reloc_section=reloc_section))
    return path


class ElfRebaseRoutingTests(unittest.TestCase):
    """Only supported relocatable classes are routed through prxload.Prx.

    The Prx rebase/relocate path understands PRX-format ELFs (ET_SCE_PRX),
    relocatable ELFs (ET_REL), and executables that still carry relocation
    sections (SHT_REL/SHT_RELA or the PSP types) at a nonzero base -- the
    -Wl,-q PSPDEV ET_EXEC fixture form included. Ordinary ET_EXEC/ET_DYN ELFs
    without relocation sections at a nonzero base must keep their legacy
    as-is behavior: no Prx instance, no address rebasing.
    """

    def _load(self, *, e_type=2, reloc_section=None, base=None):
        path = _minimal_elf_path(
            _fixture_words(), OWNER, e_type=e_type, reloc_section=reloc_section
        )
        try:
            return analyze.Elf(str(path), base)
        finally:
            shutil.rmtree(path.parent)

    def test_et_exec_nonzero_base_not_routed(self):
        elf = self._load(e_type=analyze.ET_EXEC, base=0x08800000)
        self.assertIsNone(elf.reloc)
        self.assertEqual(elf.entry, OWNER)

    def test_et_dyn_nonzero_base_not_routed(self):
        elf = self._load(e_type=3, base=0x08800000)
        self.assertIsNone(elf.reloc)
        self.assertEqual(elf.entry, OWNER)

    def test_et_exec_zero_base_not_routed(self):
        elf = self._load(e_type=analyze.ET_EXEC, base=0)
        self.assertIsNone(elf.reloc)
        self.assertEqual(elf.entry, OWNER)

    def test_et_exec_no_base_not_routed(self):
        elf = self._load(e_type=analyze.ET_EXEC, base=None)
        self.assertIsNone(elf.reloc)
        self.assertEqual(elf.entry, OWNER)

    def test_et_exec_with_rel_sections_nonzero_base_routed(self):
        elf = self._load(
            e_type=analyze.ET_EXEC, reloc_section=analyze.SHT_REL, base=0x08800000
        )
        self.assertIsNotNone(elf.reloc)
        self.assertEqual(elf.reloc.lo, 0x08800000)
        self.assertEqual(elf.entry, (OWNER + 0x08800000) & 0xFFFFFFFF)

    def test_et_exec_with_psp_relocs_nonzero_base_routed(self):
        elf = self._load(
            e_type=analyze.ET_EXEC, reloc_section=analyze.SHT_PSP_RELA, base=0x08800000
        )
        self.assertIsNotNone(elf.reloc)
        self.assertEqual(elf.reloc.lo, 0x08800000)

    def test_fixed_exec_at_its_own_load_address_not_rebased_again(self):
        # A retail fixed-address ET_EXEC keeps empty .rel.* sections, and the
        # generic profile names its lowest PT_LOAD address as the base. Its
        # addresses already are guest addresses (prxload._image_address_bias).
        lowest = min(_fixture_words())
        elf = self._load(
            e_type=analyze.ET_EXEC, reloc_section=analyze.SHT_PSP_RELA, base=lowest
        )
        self.assertIsNone(elf.reloc)
        self.assertEqual(elf.entry, OWNER)
        self.assertEqual(elf.read_at_vaddr(OWNER, 4), struct.pack("<I", 0x27BDFF90))

    def test_et_rel_with_psp_relocs_nonzero_base_routed(self):
        elf = self._load(e_type=analyze.ET_REL, reloc_section=analyze.SHT_REL, base=0x08800000)
        self.assertIsNotNone(elf.reloc)
        self.assertEqual(elf.reloc.lo, 0x08800000)

    def test_et_exec_with_rel_sections_zero_base_not_routed(self):
        elf = self._load(e_type=analyze.ET_EXEC, reloc_section=analyze.SHT_REL, base=0)
        self.assertIsNone(elf.reloc)
        self.assertEqual(elf.entry, OWNER)

    def test_et_scp_prx_nonzero_base_routed(self):
        elf = self._load(e_type=analyze.ET_SCE_PRX, base=0x08800000)
        self.assertIsNotNone(elf.reloc)
        self.assertEqual(elf.reloc.lo, 0x08800000)

    def test_et_scp_prx_zero_base_routed(self):
        elf = self._load(e_type=analyze.ET_SCE_PRX, base=0)
        self.assertIsNotNone(elf.reloc)
        self.assertEqual(elf.reloc.lo, 0)


class EntryCatalogInvariantTests(unittest.TestCase):
    def test_dual_role_entry_stops_instead_of_guessing(self):
        with self.assertRaisesRegex(RuntimeError, "DUAL-ROLE ENTRY DETECTED"):
            codegen.build_entry_catalog(
                {OWNER, RESUME}, [(OWNER, RESUME + 0x10)], profile="hst"
            )

    def test_conflicting_resume_owner_stops_instead_of_letting_the_last_slice_win(self):
        """The two slices cross-check each other rather than overwrite.

        An address reached both by a direct ``j`` and by a direct branch is
        audited twice, by two independent owner searches -- one windowed, one
        exact.  On the private image they agree on every shared address, which
        is the point of running both.  If they ever disagree, the later slice
        must not silently replace the earlier owner, because one of the two
        derivations is then wrong and the resume body would be generated
        against the wrong frame.
        """
        owner = 0x08800000
        other = 0x08800040
        resume = 0x08800010
        ranges = [(owner, owner + 0x100)]
        candidate = entry_frame_balance.DirectBranchCandidate(
            addr=resume, sources=((owner + 8, "cond"),),
            role=entry_frame_balance.CONTINUATION,
            classification=entry_frame_balance.CONTINUATION,
            owners=(other,), continuation_delta=0x20,
            contradictions=(), reason="synthetic",
        )
        with unittest.mock.patch.object(
            entry_frame_balance, "audit_direct_j_candidates",
            return_value=(entry_frame_balance.DirectJumpCandidate(
                addr=resume, sources=(owner + 8,),
                role=entry_frame_balance.CONTINUATION,
                classification=entry_frame_balance.CONTINUATION,
                owners=(owner,), continuation_delta=0x20, reason="synthetic",
            ),),
        ), unittest.mock.patch.object(
            entry_frame_balance, "audit_direct_branch_candidates",
            return_value=(candidate,),
        ):
            with self.assertRaisesRegex(RuntimeError, "CONFLICTING RESUME OWNER"):
                codegen.build_entry_catalog(
                    {owner, resume, other}, ranges, profile="none",
                    elf=object(),
                )

    def test_agreeing_slices_leave_one_resume_entry_with_both_provenances(self):
        """The same address proved twice keeps one owner and both tags."""
        owner = 0x08800000
        resume = 0x08800010
        ranges = [(owner, owner + 0x100)]
        with unittest.mock.patch.object(
            entry_frame_balance, "audit_direct_j_candidates",
            return_value=(entry_frame_balance.DirectJumpCandidate(
                addr=resume, sources=(owner + 8,),
                role=entry_frame_balance.CONTINUATION,
                classification=entry_frame_balance.CONTINUATION,
                owners=(owner,), continuation_delta=0x20, reason="synthetic",
            ),),
        ), unittest.mock.patch.object(
            entry_frame_balance, "audit_direct_branch_candidates",
            return_value=(entry_frame_balance.DirectBranchCandidate(
                addr=resume, sources=((owner + 12, "cond"),),
                role=entry_frame_balance.CONTINUATION,
                classification=entry_frame_balance.CONTINUATION,
                owners=(owner,), continuation_delta=0x20,
                contradictions=(), reason="synthetic",
            ),),
        ):
            catalog = codegen.build_entry_catalog(
                {owner, resume}, ranges, profile="none", elf=object(),
            )
        entry = catalog[resume]
        self.assertTrue(entry.resumable)
        self.assertFalse(entry.callable)
        self.assertEqual(entry.owner, owner)
        self.assertIn("direct-j", entry.provenance)
        self.assertIn("direct-branch", entry.provenance)

    def test_profile_none_returns_catalog_without_hst_entries(self):
        catalog = codegen.build_entry_catalog(
            {0x08800000, 0x08800010}, [(0x08800000, 0x08800100)], profile="none"
        )
        self.assertIn(0x08800000, catalog)
        self.assertIn(0x08800010, catalog)
        self.assertNotIn("hst-profile", catalog[0x08800000].provenance)


# A synthetic guest symbol map for the fixture above: names and addresses only, in the
# `name = 0xADDR; // type:func size:0xNN` form tools/codegen.py --symbol-map reads.
OWNER_SIZE = RESUME + 16 - OWNER
DATA_LABEL = 0x00021D00  # the fixture's pointer table (_synthetic_elf's data_addr)
UNEMITTED = 0x00030000
SYMBOL_MAP_TEXT = f"""\
# fixture functions
owner_fn = 0x{OWNER:08x}; // type:func size:0x{OWNER_SIZE:x}
adjacent_fn = 0x{ADJACENT:08x};   # no size: only the start address is named

// splat-style attributes; the data label is not a function
tail_target_fn = 0x{TAIL_TARGET:08x}; // type:func
pointer_table = 0x{DATA_LABEL:08x}; // type:data size:0x10
unemitted_fn = 0x{UNEMITTED:08x}; // type:func size:0x10
"""
MAP_NAMES = ("owner_fn", "adjacent_fn", "tail_target_fn")


def _codegen_in_process(argv: list[str]) -> SimpleNamespace:
    """Run codegen.main unmodified in this process, capturing what it prints."""
    orig_out, orig_err = sys.stdout, sys.stderr
    try:
        sys.stdout, sys.stderr = StringIO(), StringIO()
        returncode = codegen.main(["codegen.py", *argv])
        return SimpleNamespace(returncode=returncode, stdout=sys.stdout.getvalue(),
                               stderr=sys.stderr.getvalue())
    finally:
        sys.stdout, sys.stderr = orig_out, orig_err


def _run_codegen(work: Path, symbol_map_text: str | None) -> SimpleNamespace:
    """Generate the fixture into `work`, with --symbol-map when a map text is given."""
    work.mkdir()
    elf = work / "fixture.elf"
    elf.write_bytes(_synthetic_elf(
        _fixture_words(), OWNER,
        pointers=(TINY_LEAF, INDIRECT_CALLER, TAIL_SOURCE, TAIL_TARGET),
    ))
    argv = [str(elf), str(work / "fixture.c"), "--profile=hst"]
    if symbol_map_text is not None:
        (work / "symbols.txt").write_text(symbol_map_text, encoding="utf-8")
        argv.append(f"--symbol-map={work / 'symbols.txt'}")
    return _codegen_in_process(argv)


def _generated_files(work: Path) -> dict[str, str]:
    return {
        path.name: path.read_text(encoding="ascii")
        for path in sorted(work.iterdir())
        if path.name.startswith("fixture") and path.suffix in (".c", ".h", ".txt")
    }


class SymbolMapPipelineTests(unittest.TestCase):
    """--symbol-map names generated functions and changes nothing else."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = scratch_dir.TemporaryDirectory(prefix="symbol_map_")
        root = Path(cls._tmp.name)
        cls.plain_run = _run_codegen(root / "plain", None)
        cls.named_run = _run_codegen(root / "named", SYMBOL_MAP_TEXT)
        cls.unused_run = _run_codegen(
            root / "unused",
            f"unemitted_fn = 0x{UNEMITTED:08x};\n"
            f"pointer_table = 0x{DATA_LABEL:08x}; // type:data\n")
        for run in (cls.plain_run, cls.named_run, cls.unused_run):
            if run.returncode != 0:
                raise AssertionError(run.stderr + run.stdout)
        cls.named_dir = root / "named"
        cls.plain = _generated_files(root / "plain")
        cls.named = _generated_files(root / "named")
        cls.unused = _generated_files(root / "unused")

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def _chunks(self, files: dict[str, str]) -> str:
        return "\n".join(text for name, text in files.items()
                         if name.startswith("fixture_") and name[8:-2].isdigit())

    def test_named_entries_carry_their_address_and_name(self):
        chunks = self._chunks(self.named)
        for symbol in (f"f_{OWNER:08x}_owner_fn", f"f_{ADJACENT:08x}_adjacent_fn",
                       f"f_{TAIL_TARGET:08x}_tail_target_fn"):
            with self.subTest(symbol=symbol):
                self.assertIn(f"void {symbol}(CpuState *s) {{", chunks)
                self.assertIn(f"void {symbol}(CpuState *s);", self.named["fixture_funcs.h"])
        # Every reference moved with the definition: no bare spelling is left behind.
        for addr in (OWNER, ADJACENT, TAIL_TARGET):
            with self.subTest(addr=hex(addr)):
                self.assertNotRegex(chunks, rf"\bf_{addr:08x}\b")
        self.assertIn(f"f_{TINY_LEAF:08x}(CpuState *s) {{", chunks)  # unnamed stays as is

    def test_resume_body_takes_its_owners_name(self):
        chunks = self._chunks(self.named)
        self.assertIn(f"void r_{RESUME:08x}_owner_fn(CpuState *s) {{", chunks)
        self.assertIn(f"sr_register(0x{RESUME:08x}u, r_{RESUME:08x}_owner_fn);", chunks)

    def test_registration_file_hands_the_runtime_a_sorted_table(self):
        main = self.named["fixture.c"]
        rows = [f'    {{0x{OWNER:08x}u, 0x{OWNER_SIZE:08x}u, "owner_fn"}},',
                f'    {{0x{ADJACENT:08x}u, 0x00000000u, "adjacent_fn"}},',
                f'    {{0x{TAIL_TARGET:08x}u, 0x00000000u, "tail_target_fn"}},']
        self.assertIn("static const SrGuestSymbol sr_guest_symbol_table[] = {\n"
                      + "\n".join(rows) + "\n};", main)
        self.assertIn("    sr_guest_symbols_register(sr_guest_symbol_table, 3u);", main)
        self.assertNotIn("pointer_table", main)
        self.assertNotIn("unemitted_fn", main)
        self.assertIn("symbol map: 3 of 4 function name(s) name an emitted entry; "
                      "1 resume bodies take their owner's name", self.named_run.stderr)

    def test_names_change_symbol_spellings_and_nothing_else(self):
        # Undo exactly the documented renames and drop the table: the bytes must be
        # the run without a map, file for file.
        names = "|".join(MAP_NAMES)
        self.assertEqual(sorted(self.named), sorted(self.plain))
        for name, text in self.named.items():
            undone = re.sub(rf"\b([fr]_[0-9a-f]{{8}})_(?:{names})\b", r"\1", text)
            if name == "fixture.c":
                undone = re.sub(
                    r"\n\n/\* Guest function names from --symbol-map[^\n]*\n"
                    r"static const SrGuestSymbol sr_guest_symbol_table\[\] = \{\n.*?\n\};",
                    "", undone, flags=re.S)
                undone = undone.replace(
                    "    sr_guest_symbols_register(sr_guest_symbol_table, 3u);\n", "")
            with self.subTest(file=name):
                self.assertEqual(undone, self.plain[name])

    def test_a_map_that_names_no_emitted_entry_changes_no_byte(self):
        self.assertEqual(self.unused, self.plain)
        self.assertIn("symbol map: 0 of 1 function name(s)", self.unused_run.stderr)

    def test_no_map_output_mentions_no_symbol_table(self):
        self.assertNotIn("SrGuestSymbol", "".join(self.plain.values()))
        self.assertNotIn("symbol map:", self.plain_run.stderr)

    @unittest.skipUnless(CC, "gcc is required to compile the table against src/rt/recomp.h")
    def test_runtime_names_a_start_an_offset_and_nothing_outside(self):
        assert CC is not None
        main = self.named["fixture.c"]
        table = re.search(r"static const SrGuestSymbol sr_guest_symbol_table\[\] = "
                          r"\{\n.*?\n\};", main, re.S).group(0)
        harness = self.named_dir / "lookup.c"
        queries = {
            OWNER: "owner_fn",
            RESUME: f"owner_fn+0x{RESUME - OWNER:x}",  # a resume body is inside its owner
            OWNER + OWNER_SIZE - 4: f"owner_fn+0x{OWNER_SIZE - 4:x}",
            OWNER + OWNER_SIZE: "",                  # one past the size is outside
            OWNER - 4: "",                           # below the first entry
            ADJACENT: "adjacent_fn",
            ADJACENT + 4: "",                        # no size: only the start is named
            TAIL_TARGET: "tail_target_fn",
        }
        calls = "\n".join(
            f'    printf("0x%08x=[%s]\\n", 0x{pc:08x}u, sr_guest_symbol_format('
            f"sr_guest_symbol_find(sr_guest_symbol_table, n, 0x{pc:08x}u), 0x{pc:08x}u, "
            f"buf, sizeof buf));" for pc in queries)
        harness.write_text(
            f'#include "recomp.h"\n{table}\nint main(void) {{\n    char buf[64];\n'
            f"    uint32_t n = (uint32_t)(sizeof sr_guest_symbol_table / "
            f"sizeof sr_guest_symbol_table[0]);\n{calls}\n"
            f'    printf("empty=[%s]\\n", sr_guest_symbol_format('
            f"sr_guest_symbol_find(NULL, 0u, 0x{OWNER:08x}u), 0x{OWNER:08x}u, buf, sizeof buf));\n"
            f"    return 0;\n}}\n", encoding="ascii")
        exe = self.named_dir / "lookup.exe"
        compiled = subprocess.run(
            [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src" / "rt"),
             "-I", str(ROOT / "src" / "core"), str(harness), "-o", str(exe)],
            capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        # The whole generated registration file also compiles against the real header.
        checked = subprocess.run(
            [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src" / "rt"),
             "-I", str(ROOT / "src" / "core"), "-I", str(self.named_dir), "-c",
             str(self.named_dir / "fixture.c"), "-o", str(self.named_dir / "fixture.o")],
            capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        expected = "".join(f"0x{pc:08x}=[{label}]\n" for pc, label in queries.items())
        self.assertEqual(ran.stdout.replace("\r\n", "\n"), expected + "empty=[]\n")


# Sized functions whose starts are not all entries: an analyzer entry inside one, past its start,
# is named after it with `in_`.
REGION_SIZE = TAIL_SOURCE - TINY_LEAF        # TINY_LEAF .. INDIRECT_CALLER; TAIL_SOURCE is outside
TAIL_REGION = RESUME - 8                     # not an entry; its 0x10 bytes hold only RESUME
CONTAINMENT_MAP_TEXT = f"""\
region_fn = 0x{TINY_LEAF:08x}; // type:func size:0x{REGION_SIZE:x}
tail_region = 0x{TAIL_REGION:08x}; // type:func size:0x10
"""


class SymbolMapContainmentTests(unittest.TestCase):
    """An entry inside a sized map function, past its start, is f_<addr>_in_<name>."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = scratch_dir.TemporaryDirectory(prefix="symbol_map_inside_")
        root = Path(cls._tmp.name)
        cls.plain_run = _run_codegen(root / "plain", None)
        cls.inside_run = _run_codegen(root / "inside", CONTAINMENT_MAP_TEXT)
        for run in (cls.plain_run, cls.inside_run):
            if run.returncode != 0:
                raise AssertionError(run.stderr + run.stdout)
        cls.plain = _generated_files(root / "plain")
        cls.inside = _generated_files(root / "inside")

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def _chunks(self) -> str:
        return "\n".join(text for name, text in self.inside.items()
                         if name.startswith("fixture_") and name[8:-2].isdigit())

    def test_entries_inside_take_the_function_name(self):
        chunks = self._chunks()
        self.assertIn(f"void f_{TINY_LEAF:08x}_region_fn(CpuState *s) {{", chunks)
        self.assertIn(f"void f_{INDIRECT_CALLER:08x}_in_region_fn(CpuState *s) {{", chunks)
        self.assertIn(f"sr_register(0x{INDIRECT_CALLER:08x}u, f_{INDIRECT_CALLER:08x}_in_region_fn);",
                      chunks)
        # A resume body whose owner has no name takes the sized function it lies in.
        self.assertIn(f"void r_{RESUME:08x}_in_tail_region(CpuState *s) {{", chunks)
        # One past the size, and an entry no sized function holds, keep their addresses.
        self.assertIn(f"void f_{TAIL_SOURCE:08x}(CpuState *s) {{", chunks)
        self.assertIn(f"void f_{OWNER:08x}(CpuState *s) {{", chunks)
        self.assertIn("symbol map: 1 of 2 function name(s) name an emitted entry; 0 resume bodies "
                      "take their owner's name; 2 entries inside a named function take its name",
                      self.inside_run.stderr)

    def test_a_function_holding_an_entry_joins_the_runtime_table(self):
        # tail_region's own start is no entry, but the runtime must still name RESUME.
        main = self.inside["fixture.c"]
        self.assertIn(f'    {{0x{TAIL_REGION:08x}u, 0x00000010u, "tail_region"}},\n'
                      f'    {{0x{TINY_LEAF:08x}u, 0x{REGION_SIZE:08x}u, "region_fn"}},\n}};', main)
        self.assertIn("    sr_guest_symbols_register(sr_guest_symbol_table, 2u);", main)

    def test_names_change_symbol_spellings_and_nothing_else(self):
        self.assertEqual(sorted(self.inside), sorted(self.plain))
        for name, text in self.inside.items():
            undone = re.sub(r"\b([fr]_[0-9a-f]{8})_(?:in_)?(?:region_fn|tail_region)\b", r"\1", text)
            if name == "fixture.c":
                undone = re.sub(
                    r"\n\n/\* Guest function names from --symbol-map[^\n]*\n"
                    r"static const SrGuestSymbol sr_guest_symbol_table\[\] = \{\n.*?\n\};",
                    "", undone, flags=re.S)
                undone = undone.replace(
                    "    sr_guest_symbols_register(sr_guest_symbol_table, 2u);\n", "")
            with self.subTest(file=name):
                self.assertEqual(undone, self.plain[name])


class SymbolMapFileTests(unittest.TestCase):
    """The map format: ld symbol assignments, comments, and refusals with a line number."""

    def _load(self, text: str):
        with scratch_dir.TemporaryDirectory(prefix="symbol_map_file_") as tmp:
            path = Path(tmp) / "map.txt"
            path.write_text(text, encoding="utf-8")
            return codegen.load_symbol_map(str(path))

    def test_reads_names_sizes_and_comments(self):
        self.assertEqual(self._load(SYMBOL_MAP_TEXT), {
            OWNER: ("owner_fn", OWNER_SIZE),
            ADJACENT: ("adjacent_fn", 0),
            TAIL_TARGET: ("tail_target_fn", 0),
            UNEMITTED: ("unemitted_fn", 0x10),
        })

    def test_repeating_an_entry_is_harmless(self):
        line = "dup_fn = 0x00001000; // size:0x8\n"
        self.assertEqual(self._load(line + line), {0x1000: ("dup_fn", 8)})

    def test_refusals_name_the_line(self):
        cases = {
            "not an assignment": "ok_fn = 0x10;\nwhat is this\n",
            "decimal address": "ok_fn = 4096;\n",
            "missing semicolon": "ok_fn = 0x10\n",
            "name with a dot": "ok.fn = 0x10;\n",
            "wide address": "ok_fn = 0x100000000;\n",
            "two names, one address": "a_fn = 0x10;\nb_fn = 0x10;\n",
            "two sizes, one address": "a_fn = 0x10; // size:0x4\na_fn = 0x10; // size:0x8\n",
            "bad size": "a_fn = 0x10; // size:big\n",
            "zero size": "a_fn = 0x10; // size:0\n",
            "size past 4 GiB": "a_fn = 0xfffffff0; // size:0x20\n",
        }
        for label, text in cases.items():
            with self.subTest(case=label):
                with self.assertRaises(codegen.SymbolMapError) as caught:
                    self._load(text)
                self.assertRegex(str(caught.exception), r"map\.txt:\d+: ")

    def test_command_line_refuses_a_bad_map_before_the_analysis(self):
        with scratch_dir.TemporaryDirectory(prefix="symbol_map_cli_") as tmp:
            bad = Path(tmp) / "bad.txt"
            bad.write_text("not a symbol line\n", encoding="utf-8")
            elf = Path(tmp) / "fixture.elf"
            elf.write_bytes(_synthetic_elf(_fixture_words(), OWNER))
            result = _codegen_in_process([str(elf), str(Path(tmp) / "out.c"), f"--symbol-map={bad}"])
            self.assertFalse((Path(tmp) / "out.c").exists())
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNotIn("SCANNING RANGES", result.stdout)
        self.assertIn("codegen: symbol map: ", result.stderr)
        self.assertIn("bad.txt:1: ", result.stderr)

    def test_a_name_may_not_spell_a_generator_owned_symbol(self):
        # The HST main-loop stub keeps the translated body as f_000468c8_real.
        renames = codegen.symbol_map_renames({0x000468C8: ("real", 0)}, [0x000468C8], {})
        self.assertEqual(renames, {"f_000468c8": "f_000468c8_real"})
        texts = ["void f_000468c8_real(CpuState *s) {", "f_000468c8(s);"]
        self.assertEqual(codegen.symbol_map_collisions(renames, texts), ["f_000468c8_real"])
        self.assertEqual(codegen.symbol_map_collisions({"f_000468c8": "f_000468c8_loop"}, texts), [])

    def test_makefile_passes_the_map_and_regenerates_when_it_changes(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("SYMBOL_MAP_ARG := --symbol-map=$(SYMBOL_MAP)", makefile)
        self.assertIn("SYMBOL_MAP_PROFILE_LINE := $(NEWLINE)SYMBOL_MAP=$(SYMBOL_MAP)", makefile)
        rule = re.search(r"^\$\(BUILD_DIR\)/\$\(GAME_NAME\)_recomp\.c [^\n]*: ([^\n]*)\n\t([^\n]*)",
                         makefile, re.M)
        self.assertIsNotNone(rule)
        self.assertIn("$(SYMBOL_MAP)", rule.group(1).split())
        self.assertIn("$(SYMBOL_MAP_ARG)", rule.group(2).split())
        entries = re.search(r"^export NK_CODEGEN_PROFILE_ENTRIES := ([^\n]*)", makefile, re.M)
        self.assertIn("$(SYMBOL_MAP_PROFILE_LINE)", entries.group(1))


if __name__ == "__main__":
    unittest.main()
