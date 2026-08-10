# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""End-to-end regressions for callable versus resume-entry codegen semantics.

The fixture is an owned, synthetic ELF32/MIPS executable.  Its callable owner
allocates a 0x70-byte frame and then reaches an interior entry which restores
that frame and returns.  Entering the interior PC directly therefore models a
real indirect resume with the owner's frame already active.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock


ROOT = Path(__file__).resolve().parent.parent
CODEGEN = ROOT / "tools" / "codegen.py"
CC = shutil.which("gcc")
sys.path.insert(0, str(ROOT / "tools"))
import analyze  # noqa: E402
import codegen  # noqa: E402
import entry_frame_balance  # noqa: E402

OWNER = 0x00021AC0
RESUME = 0x00021C78
ADJACENT = 0x00021C88
TINY_LEAF = 0x00021C98
INDIRECT_CALLER = 0x00021CA0
TAIL_SOURCE = 0x00021CC0
TAIL_TARGET = 0x00021CD0
CALLER_SP = 0x00001000
FRAME_SIZE = 0x70


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
        with tempfile.TemporaryDirectory(prefix="entry_semantics_") as tmp:
            work = Path(tmp)
            elf = work / "entry_semantics.elf"
            generated = work / "entry_semantics.c"
            elf.write_bytes(_synthetic_elf(
                _fixture_words(), OWNER,
                pointers=(TINY_LEAF, INDIRECT_CALLER, TAIL_SOURCE, TAIL_TARGET),
            ))

            env = dict(os.environ)
            env["HST_EXTRA_SPANS"] = ""
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
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "entry_semantics_funcs.h"

static uint32_t mem[0x2000 / 4];
uint32_t test_mem_r32(uint32_t addr) {{ return mem[(addr & 0x1fffu) / 4u]; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ mem[(addr & 0x1fffu) / 4u] = value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
void dispatch(CpuState *s, uint32_t target) {{
    if (target == 0x{RESUME:08x}u) {{ r_{RESUME:08x}(s); return; }}
    if (target == 0x{TINY_LEAF:08x}u) {{ f_{TINY_LEAF:08x}(s); return; }}
    if (target == 0x{TAIL_TARGET:08x}u) {{ f_{TAIL_TARGET:08x}(s); return; }}
    abort();
}}

int main(void) {{
    CpuState resumed = {{0}};
    resumed.r[29] = 0x{CALLER_SP - FRAME_SIZE:08x}u;
    test_mem_w32(0x{CALLER_SP - 4:08x}u, 0x12345678u);
    r_{RESUME:08x}(&resumed);

    CpuState called = {{0}};
    called.r[29] = 0x{CALLER_SP:08x}u;
    called.r[31] = 0x12345678u;
    f_{OWNER:08x}(&called);

    CpuState adjacent = {{0}};
    adjacent.r[29] = 0x{CALLER_SP:08x}u;
    f_{ADJACENT:08x}(&adjacent);

    CpuState leaf = {{0}};
    leaf.r[4] = 5u;
    leaf.r[29] = 0x{CALLER_SP:08x}u;
    f_{TINY_LEAF:08x}(&leaf);

    CpuState indirect = {{0}};
    indirect.r[29] = 0x{CALLER_SP:08x}u;
    indirect.r[31] = 0x12345678u;
    f_{INDIRECT_CALLER:08x}(&indirect);

    CpuState tail = {{0}};
    tail.r[29] = 0x{CALLER_SP:08x}u;
    tail.r[31] = 0x12345678u;
    f_{TAIL_SOURCE:08x}(&tail);

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
        with tempfile.TemporaryDirectory(prefix="direct_j_semantics_") as tmp:
            work = Path(tmp)
            elf = work / "direct_j_semantics.elf"
            generated = work / "direct_j_semantics.c"
            elf.write_bytes(_synthetic_elf(words, owner))
            env = dict(os.environ)
            env["HST_EXTRA_SPANS"] = ""
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
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdlib.h>
#include "direct_j_semantics_funcs.h"
static uint32_t mem[0x2000 / 4];
uint32_t test_mem_r32(uint32_t addr) {{ return mem[(addr & 0x1fffu) / 4u]; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ mem[(addr & 0x1fffu) / 4u] = value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
void dispatch(CpuState *s, uint32_t target) {{
    if (target == 0x{resume:08x}u) {{ r_{resume:08x}(s); return; }}
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
        with tempfile.TemporaryDirectory(prefix="direct_branch_semantics_") as tmp:
            work = Path(tmp)
            elf = work / "direct_branch_semantics.elf"
            generated = work / "direct_branch_semantics.c"
            elf.write_bytes(_synthetic_elf(words, owner))
            env = dict(os.environ)
            env["HST_EXTRA_SPANS"] = ""
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
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                f"""\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "direct_branch_semantics_funcs.h"
static uint32_t mem[0x2000 / 4];
uint32_t test_mem_r32(uint32_t addr) {{ return mem[(addr & 0x1fffu) / 4u]; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ mem[(addr & 0x1fffu) / 4u] = value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
void dispatch(CpuState *s, uint32_t target) {{
    if (target == 0x{resume:08x}u) {{ r_{resume:08x}(s); return; }}
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
    f_{owner:08x}(&native);

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


if __name__ == "__main__":
    unittest.main()
