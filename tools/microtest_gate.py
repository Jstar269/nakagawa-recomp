# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

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


def find_exit_syscall_pc(elf_path):
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    from analyze import Elf
    import struct
    elf = Elf(elf_path)
    symtab = elf.sec(".symtab")
    strtab = elf.sec(".strtab")
    if not symtab or not strtab:
        raise ValueError("ELF is missing .symtab or .strtab section")
    d = elf.data
    exit_stub_addr = None
    exit_stub_size = None
    for i in range(symtab["size"] // symtab["entsz"]):
        o = symtab["off"] + i * symtab["entsz"]
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack("<IIIBBH", d[o:o + 16])
        e = strtab["off"] + st_name
        name = d[e:d.find(b"\x00", e)].decode("ascii", "replace")
        if name == "exit_stub":
            exit_stub_addr = st_value
            exit_stub_size = st_size
            break
    if exit_stub_addr is None:
        raise ValueError("missing exit_stub symbol")
    if exit_stub_size is None or exit_stub_size == 0:
        exit_stub_size = 16
    resolved_pc = None
    expected_word = 0x0008430C  # syscall 0x210c
    for offset in range(0, exit_stub_size, 4):
        addr = exit_stub_addr + offset
        w_bytes = elf.read_at_vaddr(addr, 4)
        if w_bytes and len(w_bytes) == 4:
            w = struct.unpack("<I", w_bytes)[0]
            if w == expected_word:
                resolved_pc = addr
                break
    if resolved_pc is None:
        raise ValueError("synthetic syscall 0x210c not found within exit_stub range")
    return resolved_pc


def first_syscall_step(oracle_path, exit_pc):
    with open(oracle_path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line or line[0] == "#":
                continue
            parts = line.split()
            if len(parts) < 2 or not parts[1].startswith("pc="):
                continue
            pc = int(parts[1][3:], 16)
            if pc == exit_pc:
                if len(parts) >= 3 and parts[2].startswith("op="):
                    op = int(parts[2][3:], 16)
                    if op == 0x0008430C:
                        return int(parts[0], 10)
    return None


def write_truncated(oracle_path, out_path, count):
    with open(oracle_path, "r", encoding="utf-8") as src, \
         open(out_path, "w", encoding="utf-8", newline="\n") as dst:
        for line in src:
            line_stripped = line.rstrip("\r\n")
            if line_stripped and line_stripped[0] == "#":
                dst.write(line_stripped + "\n")
                continue
            parts = line_stripped.split()
            if not parts:
                continue
            if int(parts[0], 10) >= count:
                break
            dst.write(line_stripped + "\n")


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
