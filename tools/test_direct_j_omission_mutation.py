# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compiled mutation proof for omitted direct-j execution boundaries."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
CODEGEN = ROOT / "tools" / "codegen.py"
CC = shutil.which("gcc")


def _synthetic_elf(words: dict[int, int], entry: int) -> bytes:
    """Build the small executable used by the direct-j omission campaign."""
    import struct

    lo = min(words)
    hi = max(words) + 4
    text_size = hi - lo
    data_addr = (hi + 0xFF) & ~0xFF
    segment = bytearray(data_addr - lo + 4)
    for address, word in words.items():
        struct.pack_into("<I", segment, address - lo, word & 0xFFFFFFFF)
    data_off = 0x100
    shstr = b"\0.text\0.data\0.shstrtab\0"
    shstr_off = data_off + len(segment)
    shoff = (shstr_off + len(shstr) + 3) & ~3
    blob = bytearray(shoff + 4 * 40)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    blob[:52] = ident + struct.pack(
        "<HHIIIIIHHHHHH", 2, 8, 1, entry, 52, shoff, 0x50001000,
        52, 32, 1, 40, 4, 3,
    )
    blob[52:84] = struct.pack(
        "<8I", 1, data_off, lo, lo, len(segment), len(segment), 7, 0x1000
    )
    blob[data_off:data_off + len(segment)] = segment
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, lo, data_off, text_size, 0, 0, 4, 0),
        (7, 1, 3, data_addr, data_off + data_addr - lo, 4, 0, 0, 4, 0),
        (13, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),
    ]
    for index, section in enumerate(sections):
        struct.pack_into("<10I", blob, shoff + index * 40, *section)
    return bytes(blob)


def _write_harness(work: Path, target: int, owner: int) -> None:
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
void dispatch_call(CpuState *s, uint32_t target, uint32_t resume_pc);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
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
#include "direct_j_funcs.h"

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
    f_{owner:08x}(&state);
    printf("dispatch_hits=%d v0=0x%08x v1=0x%08x\\n",
           dispatch_hits, state.r[2], state.r[3]);
    return dispatch_hits == 1 && state.r[2] == 0x22u && state.r[3] == 0x55u ? 0 : 1;
}}
""",
        encoding="ascii",
    )


def _generate_compile_run(codegen_path: Path, work: Path, target: int, owner: int) -> tuple[int, str, int, str]:
    elf = work / "direct_j.elf"
    generated = work / "direct_j.c"
    elf.write_bytes(_synthetic_elf({
        owner: 0x08000410,       # direct j target
        owner + 4: 0x24030011,   # delay slot: v1 = 0x11
        owner + 8: 0x0C000410,   # dead jal discovery anchor
        owner + 12: 0x24000000,
        target: 0x24020022,
        target + 4: 0x03E00008,
        target + 8: 0x24630044,
    }, owner))
    env = dict(os.environ)
    env["HST_EXTRA_SPANS"] = ""
    env["PYTHONPATH"] = str(ROOT / "tools") + os.pathsep + env.get("PYTHONPATH", "")
    generated_result = subprocess.run(
        [sys.executable, str(codegen_path), str(elf), str(generated),
         "--profile=hst", "--funcs-per-chunk=1", f"--omit-aot=0x{target:08x}"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if generated_result.returncode != 0:
        return generated_result.returncode, generated_result.stderr + generated_result.stdout, 1, ""

    _write_harness(work, target, owner)
    chunks = sorted(work.glob("direct_j_[0-9]*.c"))
    exe = work / "direct_j.exe"
    compile_result = subprocess.run(
        [
            CC, "-std=c11", "-O0", "-Wall", "-Wextra", "-I", str(work),
            str(work / "harness.c"), str(generated),
            *(str(path) for path in chunks), "-lm", "-o", str(exe),
        ],
        cwd=ROOT, capture_output=True, text=True,
    )
    if compile_result.returncode != 0:
        return 0, generated_result.stderr + generated_result.stdout, compile_result.returncode, compile_result.stderr + compile_result.stdout
    run_result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
    return 0, generated_result.stderr + generated_result.stdout, run_result.returncode, run_result.stderr + run_result.stdout


@unittest.skipUnless(CC, "gcc is required for the compiled mutation proof")
class DirectJOmissionMutationTests(unittest.TestCase):
    def test_direct_j_boundary_mutant_is_compiled_and_killed(self):
        assert CC is not None
        owner = 0x00001000
        target = 0x00001040
        original = CODEGEN.read_text(encoding="utf-8")
        anchor = (
            "        if target_pc in dispatch_boundaries:\n"
            "            return True\n"
            "        owner = resume_owners.get(target_pc)"
        )
        self.assertIn(anchor, original, "direct-j dispatch-boundary mutation anchor drifted")
        mutant = original.replace(anchor, "        owner = resume_owners.get(target_pc)", 1)
        self.assertNotEqual(mutant, original)

        with tempfile.TemporaryDirectory(prefix="direct_j_omission_mut_") as tmp:
            root = Path(tmp)
            pristine_dir = root / "pristine"
            mutant_dir = root / "mutant"
            pristine_dir.mkdir()
            mutant_dir.mkdir()

            pristine_compile, pristine_gen, pristine_run, pristine_out = _generate_compile_run(
                CODEGEN, pristine_dir, target, owner
            )
            self.assertEqual(pristine_compile, 0, pristine_gen + pristine_out)
            self.assertEqual(pristine_run, 0, "pristine direct-j fixture failed\n" + pristine_out)
            self.assertIn("dispatch_hits=1", pristine_out)

            mutant_codegen = root / "mutant_codegen.py"
            mutant_codegen.write_text(mutant, encoding="utf-8", newline="\n")
            mutant_compile, mutant_gen, mutant_run, mutant_out = _generate_compile_run(
                mutant_codegen, mutant_dir, target, owner
            )
            self.assertEqual(
                mutant_compile, 0,
                "MUTANT_BUILD_FAILED: direct-j mutant did not compile cleanly\n"
                + mutant_gen + mutant_out,
            )
            self.assertNotEqual(
                mutant_run, 0,
                "MUTANT_SURVIVED: omitted direct-j target was not semantically required\n"
                + mutant_out,
            )
            self.assertIn(
                "dispatch_hits=0", mutant_out,
                "MUTANT_NON_SEMANTIC_FAILURE: direct-j mutant did not inline the target\n"
                + mutant_out,
            )
            print("direct-j-omission: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")


if __name__ == "__main__":
    unittest.main()
