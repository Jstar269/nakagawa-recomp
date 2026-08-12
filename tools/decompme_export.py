#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""decompme_export.py - export one guest function as a decomp.me-ready bundle.

Read-only, offline analysis tool for the decompilation track (Product 2). It
reuses tools/analyze.py to locate a function in a decrypted PSP ELF/PRX and emits
a bundle from which a decomp.me *scratch* can be created - WITHOUT touching the
recompiler (codegen.py) and WITHOUT any network access.

Output (default under build/<game>/decompme/f_<addr>/, which is gitignored):

    metadata.json  provenance: address, size, sha256, source-input sha256, commit
    context.c      decomp.me `context` (base PSP typedefs; extend via the API DB)
    function.bin   raw guest function bytes
    target.o       minimal little-endian MIPS ELF object (decomp.me `target_obj`)
    target.s       GNU-syntax disassembly - ONLY when --objdump <mips-objdump> is
                   given (this host may ship no MIPS objdump; see the integration
                   doc). Without it the bundle relies on target.o, which decomp.me
                   disassembles server-side.
    starter.c      empty starter carrying the privacy header

PRIVACY: the emitted bytes/asm are derived from a retail game. This tool never
uploads them; it writes only into the (gitignored) output directory. Creating a
decomp.me scratch - especially on the public service - is a separate, deliberate
step. Treat target assembly as proprietary-derived until you decide otherwise.
See docs/DECOMPME_INTEGRATION.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = Path(TOOLS_DIR).parent


def sha256_hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def git_commit() -> str | None:
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(REPO),
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0:
            return r.stdout.strip()
    except Exception:
        pass
    return None


def func_extent(addr: int, starts, ranges):
    """(start, end) for the function at `addr`: bounded by the next function start
    in the same executable range, else the range end. Approximate (matches the
    analyzer's own model); decomp.me matching can later refine boundaries (#51)."""
    rng = next(((lo, hi) for lo, hi in ranges if lo <= addr < hi), None)
    if rng is None:
        return None
    lo, hi = rng
    later = sorted(s for s in starts if addr < s < hi)
    end = later[0] if later else hi
    return addr, end


def gen_context_c(nids=None) -> str:
    """decomp.me `context`: base PSP typedefs. Prototypes from the NID->signature
    API database and recovered game structs are progressively added later (#DB)."""
    lines = [
        "/* decomp.me context - base PSP typedefs. Extend with the PSP API database",
        " * (NID -> prototype) and recovered game types as decompilation proceeds.",
        " * See docs/DECOMPME_INTEGRATION.md. */",
        "typedef unsigned char  u8;",
        "typedef unsigned short u16;",
        "typedef unsigned int   u32;",
        "typedef signed char    s8;",
        "typedef signed short   s16;",
        "typedef signed int     s32;",
        "typedef float          f32;",
        "typedef double         f64;",
        "",
    ]
    if nids:
        lines.append("/* Imported NIDs referenced nearby (prototypes: TODO via API DB): */")
        for lib, nid in nids:
            lines.append(f"/*   {lib}  {nid:#010x} */")
        lines.append("")
    return "\n".join(lines)


# ELF constants
_EM_MIPS = 8
_ET_REL = 1
_EF_MIPS32_O32 = 0x50001000  # EF_MIPS_ARCH_32 | EF_MIPS_ABI_O32; decomp.me overrides -march


def min_mips_elf_object(func_bytes: bytes, name: str = "func",
                        e_flags: int = _EF_MIPS32_O32) -> bytes:
    """A minimal 32-bit little-endian ET_REL MIPS object with `func_bytes` in a
    single .text section and a global STT_FUNC symbol `name`. This is decomp.me's
    `target_obj`; it disassembles the object with its own PSP objdump, so we do not
    need a local MIPS objdump to produce a usable target."""
    name_b = name.encode("ascii", "replace")
    shstr = b"\x00.text\x00.shstrtab\x00.symtab\x00.strtab\x00"
    n_text = shstr.index(b".text\x00")
    n_shstr = shstr.index(b".shstrtab\x00")
    n_symtab = shstr.index(b".symtab\x00")
    n_strtab = shstr.index(b".strtab\x00")
    strtab = b"\x00" + name_b + b"\x00"

    def sym(st_name, st_value, st_size, st_info, st_shndx):
        return struct.pack("<IIIBBH", st_name, st_value, st_size, st_info, 0, st_shndx)

    # symbol 0 = null (local); symbol 1 = the function (global, in .text = section 1)
    symtab = sym(0, 0, 0, 0, 0) + sym(1, 0, len(func_bytes), (1 << 4) | 2, 1)

    def align4(n):
        return (n + 3) & ~3

    text_off = 52
    shstr_off = text_off + len(func_bytes)
    strtab_off = shstr_off + len(shstr)
    symtab_off = align4(strtab_off + len(strtab))
    sh_off = align4(symtab_off + len(symtab))
    total = sh_off + 5 * 40

    buf = bytearray(total)
    e_ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8  # 32-bit, LE, v1, SYSV
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        _ET_REL, _EM_MIPS, 1, 0, 0, sh_off, e_flags, 52, 0, 0, 40, 5, 2)
    buf[0:52] = ehdr
    buf[text_off:text_off + len(func_bytes)] = func_bytes
    buf[shstr_off:shstr_off + len(shstr)] = shstr
    buf[strtab_off:strtab_off + len(strtab)] = strtab
    buf[symtab_off:symtab_off + len(symtab)] = symtab

    shdrs = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),                                             # null
        (n_text, 1, 6, 0, text_off, len(func_bytes), 0, 0, 4, 0),                   # .text PROGBITS ALLOC|EXEC
        (n_shstr, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),                       # .shstrtab
        (n_symtab, 2, 0, 0, symtab_off, len(symtab), 4, 1, 4, 16),                   # .symtab link=4 info=1
        (n_strtab, 3, 0, 0, strtab_off, len(strtab), 0, 0, 1, 0),                    # .strtab
    ]
    for i, sh in enumerate(shdrs):
        buf[sh_off + i * 40: sh_off + (i + 1) * 40] = struct.pack("<10I", *sh)
    return bytes(buf)


def export_function(elf_path: str, addr: int, base: int, outdir: Path,
                    name: str, objdump: str | None = None) -> dict:
    """Locate the function at `addr`, extract its bytes, and write the bundle.
    Returns the metadata dict. Read-only w.r.t. the repo; never touches the net."""
    if TOOLS_DIR not in sys.path:
        sys.path.insert(0, TOOLS_DIR)
    import analyze  # noqa: E402  (repo tool, resolved via sys.path above)

    elf = analyze.Elf(elf_path, base=base)
    starts, ranges = analyze.analyze(elf, extra_spans=analyze.analyzer_span_from_env())
    if addr not in set(starts):
        near = sorted(s for s in starts if abs(s - addr) <= 0x400)
        hint = ", ".join(f"{s:#x}" for s in near[:6]) or "none within 0x400"
        raise SystemExit(f"{addr:#x} is not a known function start (nearby starts: {hint})")
    ext = func_extent(addr, starts, ranges)
    if ext is None:
        raise SystemExit(f"{addr:#x} is not in any executable range")
    start, end = ext
    size = end - start
    data = elf.read_at_vaddr(start, size)
    if data is None or len(data) < size:
        raise SystemExit(f"could not read {size} bytes of function at {start:#x}")

    src_sha = sha256_hex(Path(elf_path).read_bytes())
    meta = {
        "module": "main",
        "address": f"{start:#010x}",
        "size": size,
        "sha256": sha256_hex(data),
        "source_input_sha256": src_sha,
        "nakagawa_commit": git_commit(),
        "base": base,
        "extent_method": "next-start-or-range-end",
        "function_confidence": "start",
    }

    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "function.bin").write_bytes(data)
    (outdir / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    (outdir / "context.c").write_text(gen_context_c(), encoding="utf-8")
    (outdir / "target.o").write_bytes(min_mips_elf_object(data, name=name))
    (outdir / "starter.c").write_text(
        "/* Retail-derived. Do not upload externally without a deliberate decision.\n"
        f" * {name} @ {start:#010x}, {size} bytes. Paste matching C here. */\n",
        encoding="utf-8")

    if objdump:
        try:
            r = subprocess.run([objdump, "-d", "-Mreg-names=32", str(outdir / "target.o")],
                               capture_output=True, text=True, timeout=60)
            if r.returncode == 0 and r.stdout:
                (outdir / "target.s").write_text(r.stdout, encoding="utf-8")
            else:
                (outdir / "target.s.note").write_text(
                    "objdump failed; use target.o (decomp.me target_obj) instead.\n"
                    + (r.stderr or ""), encoding="utf-8")
        except Exception as e:  # pragma: no cover - env dependent
            (outdir / "target.s.note").write_text(f"objdump unavailable: {e}\n", encoding="utf-8")
    else:
        (outdir / "target.s.note").write_text(
            "No --objdump given. Upload target.o as decomp.me `target_obj`, or pass a\n"
            "MIPS objdump (--objdump) to emit GNU-syntax target.s. See docs/DECOMPME_INTEGRATION.md.\n",
            encoding="utf-8")
    return meta


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Export a guest function as a decomp.me bundle (read-only, offline).")
    ap.add_argument("elf", help="decrypted PSP ELF/PRX (a private input; never committed)")
    ap.add_argument("--function", required=True, help="function start address, e.g. 0x0005a648")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0, help="load base (HST uses 0)")
    ap.add_argument("--game", default="hst", help="game id for the default output dir")
    ap.add_argument("--output", help="output directory (default build/<game>/decompme/f_<addr>)")
    ap.add_argument("--name", help="symbol name for the target object (default f_<addr>)")
    ap.add_argument("--objdump", help="path to a MIPS objdump to also emit GNU-syntax target.s")
    args = ap.parse_args(argv)

    addr = int(args.function, 0)
    name = args.name or f"f_{addr:08x}"
    outdir = Path(args.output) if args.output else (REPO / "build" / args.game / "decompme" / f"f_{addr:08x}")

    sys.stderr.write(
        "[decompme_export] PRIVACY: output is retail-derived and written only to the\n"
        "  gitignored build dir. It is NOT uploaded. Creating a public decomp.me scratch\n"
        "  from it is a separate, deliberate decision. See docs/DECOMPME_INTEGRATION.md.\n")

    meta = export_function(args.elf, addr, args.base, outdir, name, objdump=args.objdump)
    print(f"exported {name} ({meta['size']} bytes, sha {meta['sha256'][:12]}) -> {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
