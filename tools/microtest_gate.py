# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Per-opcode differential gate for a CRT-free test module.
#
# The module runs a block of self-contained test instructions then calls sceKernelExitGame.
# Everything before that exit syscall is pure CPU execution with no HLE, so the reference
# interpreter must reproduce the reference interpreter trace exactly up to that point. This script:
#   1. finds the first syscall step in the reference interpreter trace (opcode 0, funct 0x0c),
#   2. truncates the trace to the steps strictly before it,
#   3. runs the reference interpreter for that many steps,
#   4. requires the two traces to be byte-identical (zero divergences).
#
# Usage: microtest_gate.py <run_elf.exe> <module.elf> <oracle.trace> <workdir>
# Exit 0 only when every pre-syscall instruction matches the reference interpreter.

import subprocess
import sys
import os


try:
    from .codegen_gate import find_exit_syscall_pc
    from .codegen_gate import first_syscall_step as _first_syscall_step
    from .codegen_gate import truncate as _truncate
except ImportError:
    from codegen_gate import find_exit_syscall_pc
    from codegen_gate import first_syscall_step as _first_syscall_step
    from codegen_gate import truncate as _truncate


def first_syscall_step(oracle_path, exit_pc):
    return _first_syscall_step(oracle_path, exit_pc)


def write_truncated(oracle_path, out_path, count):
    return _truncate(oracle_path, out_path, count)


def main(argv):
    if len(argv) != 5:
        sys.stderr.write("usage: microtest_gate.py <run_elf.exe> <module.elf> <oracle.trace> <workdir>\n")
        return 2
    run_elf, module, oracle, workdir = argv[1:]
    run_elf = os.path.abspath(run_elf)
    module = os.path.abspath(module)
    oracle = os.path.abspath(oracle)
    os.makedirs(workdir, exist_ok=True)

    try:
        exit_pc = find_exit_syscall_pc(module)
    except Exception as e:
        sys.stderr.write(f"ERROR: Exit resolution failed: {e}\n")
        return 1
    syscall_step = first_syscall_step(oracle, exit_pc)
    if syscall_step is None:
        sys.stderr.write("no syscall (exit) found in reference interpreter trace; module did not reach its exit\n")
        return 2
    print(f"first exit syscall at trace step {syscall_step}; comparing the {syscall_step} preceding instructions")

    trunc = os.path.join(workdir, "oracle_pre_exit.trace")
    write_truncated(oracle, trunc, syscall_step)

    mine = os.path.join(workdir, "ref.trace")
    subprocess.run([run_elf, module, oracle, mine, str(syscall_step)], check=True)

    here = os.path.dirname(os.path.abspath(__file__))
    result = subprocess.run([sys.executable, os.path.join(here, "tracediff.py"), trunc, mine])
    if result.returncode == 0:
        print("microtest gate OK: all pre-exit instructions match reference interpreter")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
