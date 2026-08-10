# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the host translation used by the production PSP smoke oracle.

The smoke producer is deliberately built from the same source-owned PSP ELF
that is staged to the physical PSP.  This helper does not evaluate the answer
or emit an oracle record; it only locates the named guest function, runs the
normal recompiler, and writes a tiny adapter which lets the existing
production-HLE selftest invoke that generated body.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
ENTRY_NAME = "nakagawa_psp_oracle_sum_u32"
FUNCTION_RE = re.compile(r"^void f_([0-9a-f]{8})\(CpuState \*s\);$", re.MULTILINE)


def _symbol_address(elf_path: Path, symbol_name: str) -> int:
    """Return the address of one STT_FUNC symbol from a PSP ELF symtab."""

    data = elf_path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError(f"{elf_path}: expected a little-endian ELF32 input")
    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize = struct.unpack_from("<H", data, 46)[0]
    shnum = struct.unpack_from("<H", data, 48)[0]
    shstrndx = struct.unpack_from("<H", data, 50)[0]
    if not shoff or not shentsize or not shnum or shstrndx >= shnum:
        raise ValueError(f"{elf_path}: no section table for named symbol lookup")

    sections: list[tuple[int, int, int, int, int, int]] = []
    for index in range(shnum):
        offset = shoff + index * shentsize
        if offset + 40 > len(data):
            raise ValueError(f"{elf_path}: truncated section header {index}")
        name, typ, _flags, _addr, section_offset, size = struct.unpack_from(
            "<IIIIII", data, offset + 0
        )
        link = struct.unpack_from("<I", data, offset + 24)[0]
        entsize = struct.unpack_from("<I", data, offset + 36)[0]
        sections.append((name, typ, section_offset, size, link, entsize))

    shstr_name, _, shstr_offset, shstr_size, _, _ = sections[shstrndx]
    del shstr_name
    shstr = data[shstr_offset : shstr_offset + shstr_size]

    def section_name(name_offset: int) -> str:
        if name_offset >= len(shstr):
            return ""
        end = shstr.find(b"\0", name_offset)
        return shstr[name_offset : end if end >= 0 else len(shstr)].decode("ascii", "replace")

    symtab_index = next((i for i, section in enumerate(sections) if section_name(section[0]) == ".symtab"), None)
    strtab_index = next((i for i, section in enumerate(sections) if section_name(section[0]) == ".strtab"), None)
    if symtab_index is None or strtab_index is None:
        raise ValueError(f"{elf_path}: no .symtab/.strtab for {symbol_name}")
    _, _, sym_offset, sym_size, _, sym_entsize = sections[symtab_index]
    _, _, str_offset, str_size, _, _ = sections[strtab_index]
    if sym_entsize < 16 or sym_offset + sym_size > len(data):
        raise ValueError(f"{elf_path}: malformed symbol table")
    strings = data[str_offset : str_offset + str_size]

    matches: list[int] = []
    for offset in range(sym_offset, sym_offset + sym_size, sym_entsize):
        if offset + 16 > len(data):
            raise ValueError(f"{elf_path}: truncated symbol entry")
        name_offset, value, size, info, _other, _section = struct.unpack_from("<IIIBBH", data, offset)
        del size
        if (info & 0x0F) != 2 or name_offset >= len(strings):  # STT_FUNC
            continue
        end = strings.find(b"\0", name_offset)
        name = strings[name_offset : end if end >= 0 else len(strings)].decode("ascii", "replace")
        if name == symbol_name:
            matches.append(value)
    if len(matches) != 1:
        raise ValueError(f"{elf_path}: expected one {symbol_name} function, found {len(matches)}")
    return matches[0]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--symbol", default=ENTRY_NAME)
    parser.add_argument("--funcs-per-chunk", type=int, default=10000)
    args = parser.parse_args(argv)

    elf_path = args.elf.resolve()
    out_dir = args.out_dir.resolve()
    if not elf_path.is_file():
        parser.error(f"ELF does not exist: {elf_path}")
    if args.funcs_per_chunk <= 0:
        parser.error("--funcs-per-chunk must be positive")

    out_dir.mkdir(parents=True, exist_ok=True)
    for path in out_dir.glob("smoke_recomp*"):
        if path.is_file():
            path.unlink()
    entry = _symbol_address(elf_path, args.symbol)
    base = out_dir / "smoke_recomp.c"
    command = [
        sys.executable,
        str(TOOLS / "codegen.py"),
        str(elf_path),
        str(base),
        "--base=0",
        f"--funcs-per-chunk={args.funcs_per_chunk}",
    ]
    subprocess.run(command, cwd=ROOT, check=True)

    header = base.with_name("smoke_recomp_funcs.h")
    declared = {int(match, 16) for match in FUNCTION_RE.findall(header.read_text(encoding="utf-8"))}
    if entry not in declared:
        raise ValueError(f"generated translation omitted {args.symbol} at 0x{entry:08x}")
    generated_chunks = sorted(out_dir.glob("smoke_recomp_*.c"))
    if not generated_chunks:
        raise ValueError("codegen produced no generated chunks")

    adapter = out_dir / "smoke_entry.c"
    adapter.write_text(
        "/* Generated by tools/psp_oracle/build_nakagawa_smoke.py. */\n"
        '#include "recomp.h"\n'
        f"void f_{entry:08x}(CpuState *s);\n\n"
        "uint32_t sr_psp_oracle_smoke_sum(CpuState *s, uint32_t count) {\n"
        "    s->r[4] = count;\n"
        f"    f_{entry:08x}(s);\n"
        "    return s->r[2];\n"
        "}\n",
        encoding="utf-8",
        newline="\n",
    )
    manifest = {
        "schema": 1,
        "source_elf": str(elf_path),
        "source_elf_sha256": _sha256(elf_path),
        "guest_symbol": args.symbol,
        "guest_address": f"0x{entry:08x}",
        "generated_header": str(header),
        "generated_chunks": [str(path) for path in generated_chunks],
        "adapter": str(adapter),
    }
    (out_dir / "smoke_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
