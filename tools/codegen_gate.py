# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Phase 3 codegen gate: generate C for a module, compile it with the runtime + driver, run
# it with tracing, and require the trace to match the internal reference-interpreter trace
# up to the first explicit exit syscall (the trace is truncated there, where the reference
# interpreter stops via StopReason::kSyscall and the generated code longjmps).
#
# Usage: codegen_gate.py <elf> <oracle.trace> <workdir>

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    return r.returncode


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


def first_syscall_step(oracle, exit_pc):
    with open(oracle, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line[0] == "#":
                continue
            p = line.split()
            if len(p) < 2 or not p[1].startswith("pc="):
                continue
            pc = int(p[1][3:], 16)
            if pc == exit_pc:
                if len(p) >= 3 and p[2].startswith("op="):
                    op = int(p[2][3:], 16)
                    if op == 0x0008430C:
                        return int(p[0])
    return None


def truncate(oracle, out, count):
    with open(oracle, "r", encoding="utf-8") as src, open(out, "w", encoding="utf-8", newline="\n") as dst:
        for line in src:
            line_stripped = line.rstrip("\r\n")
            if line_stripped and line_stripped[0] == "#":
                dst.write(line_stripped + "\n")
                continue
            p = line_stripped.split()
            if not p:
                continue
            if int(p[0]) >= count:
                break
            dst.write(line_stripped + "\n")


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: codegen_gate.py <elf> <oracle.trace> <workdir>\n")
        return 2
    elf, oracle, workdir = (os.path.abspath(a) for a in argv[1:])
    os.makedirs(workdir, exist_ok=True)
    gen = os.path.join(workdir, "gen.c")
    drv_ext = ".exe" if sys.platform.startswith("win") else ""
    drv = os.path.join(workdir, "driver" + drv_ext)
    mine = os.path.join(workdir, "my.trace")
    trunc = os.path.join(workdir, "oracle_pre_hle.trace")

    env = dict(os.environ)
    tmp = os.path.join(ROOT, ".tmp")
    os.makedirs(tmp, exist_ok=True)
    env["TMPDIR"] = env["TMP"] = env["TEMP"] = tmp.replace("\\", "/")

    if run([sys.executable, os.path.join(ROOT, "tools", "codegen.py"), "--static-verify", elf, gen]):
        return 1
    rt = os.path.join(ROOT, "src", "rt")
    cc = os.environ.get("CC", "gcc")

    # Generate the generic runtime title configuration (all optional bindings
    # disabled; no manifest) into a dedicated config directory in workdir.
    config_dir = os.path.join(workdir, "title-config")
    os.makedirs(config_dir, exist_ok=True)
    config_header = os.path.join(config_dir, "sr_title_config.h")
    if run([sys.executable, os.path.join(ROOT, "tools", "title_runtime_config.py"), "--output", config_header]):
        return 1

    # codegen.py writes the per-chunk registration files gen_0.c, gen_1.c, ...
    # alongside gen.c (each defines sr_register_chunk_i(), called by sr_register_all
    # in gen.c). They must be compiled and linked or the link fails with an
    # undefined sr_register_chunk_0.
    import glob
    base = os.path.splitext(gen)[0]
    chunk_srcs = sorted(glob.glob(base + "_*.c"))
    # The headless microtest link set is: generated chunks + recomp core +
    # guest_interp.c (recomp.c's sr_lookup()/dispatch() consult the exec-span
    # registry and interpreter floor it implements) + the VFPU table loader
    # (vfpu_tables.c, which owns the table globals recomp.c references) +
    # driver + title_config.c + tools/gate_stub.c.  sched.c / sr_coro.c are
    # intentionally omitted because the microtest never enters the scheduler
    # (no --sched / --gui flags), and those TUs would require SDL3 headers on a
    # headless Linux runner.  gate_stub.c provides the minimal dead-symbol
    # definitions the linker needs.
    extra = os.environ.get("CG_EXTRA_OBJS", "").split()
    cflags = os.environ.get("CG_EXTRA_CFLAGS", "").split()
    if run([cc, "-O0", "-w", "-fno-var-tracking", "-D_CRT_SECURE_NO_WARNINGS",
            "-DSR_INSTRUCTION_TRACE", "-DSR_GATE_BUILD", "-I", rt, "-I", config_dir, *cflags,
            "-o", drv, gen, *chunk_srcs, os.path.join(rt, "recomp.c"),
            os.path.join(rt, "guest_interp.c"),
            os.path.join(rt, "vfpu_tables.c"), os.path.join(rt, "driver.c"),
            os.path.join(rt, "title_config.c"),
            os.path.join(ROOT, "tools", "gate_stub.c"), *extra, "-lm"], env=env):
        return 1

    # Run the compiled driver while capturing stdout/stderr to inspect for SV_MISMATCH
    proc = subprocess.run([drv, elf, oracle, mine], capture_output=True, text=True, env=env)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return proc.returncode

    if "SV_MISMATCH" in proc.stderr:
        sys.stderr.write("ERROR: Static verification mismatch detected!\n")
        return 1


    exit_pc = find_exit_syscall_pc(elf)
    s = first_syscall_step(oracle, exit_pc)
    if s is None:
        sys.stderr.write("no syscall in oracle trace\n")
        return 2
    truncate(oracle, trunc, s)
    print(f"comparing the {s} pre-exit instructions")
    rc = run([sys.executable, os.path.join(ROOT, "tools", "tracediff.py"), trunc, mine])
    if rc == 0:
        print("codegen gate OK: generated C matches internal reference-interpreter trace")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
