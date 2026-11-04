#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Code a game installs at run time: captures, and their translation (#1933).

Some games copy code into memory the executable reserves (its BSS, or the heap) from their own
data files, then call it. No translation covers that code, so the call is a dispatch miss and the
run stops by name (the PR #118 floor). With ``SR_CODE_CAPTURE=<folder>`` set, the runtime also
writes the bytes around the target to that folder before it stops (src/rt/recomp.c,
``code_capture_note``). This tool reads those files and makes each one an ELF type 0xFFA8
fixed-address overlay, which ``tools/codegen.py --code-capture=PATH`` translates like the overlays
of #1938: the analysis is seeded only from the addresses the runs called, the translation's
identity is the words it translated, and the runtime binds it at instruction-cache maintenance
only while guest memory holds exactly those words. Different bytes are never bound; a call into
them misses again and writes a capture of its own.

A capture holds bytes of the user's own game. It is private data of the same class as a
decrypted module: it stays in the title's private folder and is never committed or published
(AGENTS.md Section 5). Tests use synthetic captures only.

File format (little-endian), ``code_capture_<target>_<fnv1a64 of the bytes>.nkcap``::

    0   8  magic  b"NKCODE01"
    8   4  load       guest address of the first byte
    12  4  size       number of bytes after the header (a non-zero multiple of 4)
    16  4  target     the address the run called, inside [load, load + size)
    20  4  pc         the guest pc when the run stopped
    24  4  ra         $ra when the run stopped
    28  4  reserved   zero
    32     the bytes

    code_capture.py info CAPTURE...
    code_capture.py translate --out UNIT.c CAPTURE...

A CAPTURE is a file or a folder (every ``*.nkcap`` in it). ``translate`` writes one C translation
unit with the images' bodies and descriptors and ``sr_register_code_captures()``, which registers
them; the native self-test links it. A title build passes the folder to ``codegen.py`` instead.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"NKCODE01"
HEADER = struct.Struct("<8s6I")
SUFFIX = ".nkcap"
FIXED_OVERLAY_ELF_TYPE = 0xFFA8   # analyze.FIXED_OVERLAY_ELF_TYPE
_ELF_HEADER_BYTES = 52
_PROGRAM_HEADER_BYTES = 32


class CaptureError(ValueError):
    """A capture file that is not in the documented format."""


@dataclass(frozen=True)
class Capture:
    load: int
    target: int
    pc: int
    ra: int
    data: bytes
    source: str = "<capture>"

    @property
    def end(self):
        return self.load + len(self.data)


@dataclass
class CaptureImage:
    """Captures whose windows overlap and agree byte for byte, as one image."""
    load: int
    data: bytearray
    targets: set = field(default_factory=set)
    sources: list = field(default_factory=list)

    @property
    def end(self):
        return self.load + len(self.data)

    @property
    def name(self):
        return f"code_capture_{self.load:08x}_{fnv1a64(self.data):016x}"


def fnv1a64(data):
    """FNV-1a, 64-bit: codegen.fnv1a64, which sr_module_code_fnv1a64 mirrors at run time."""
    import codegen   # here, not at the top: codegen imports this module
    return codegen.fnv1a64(data)


def parse_capture(blob, source="<capture>"):
    blob = bytes(blob)
    if len(blob) < HEADER.size:
        raise CaptureError(f"{source}: shorter than the {HEADER.size}-byte header")
    magic, load, size, target, pc, ra, reserved = HEADER.unpack_from(blob)
    if magic != MAGIC:
        raise CaptureError(f"{source}: not a code capture (magic {magic!r})")
    if reserved != 0:
        raise CaptureError(f"{source}: reserved header word is 0x{reserved:08x}, not zero")
    if size == 0 or size & 3 or load & 3 or target & 3:
        raise CaptureError(f"{source}: load, size or target is not word aligned, or the "
                           "capture is empty")
    if HEADER.size + size != len(blob):
        raise CaptureError(f"{source}: header names 0x{size:x} bytes, the file holds "
                           f"0x{len(blob) - HEADER.size:x}")
    if load + size > 0x100000000:
        raise CaptureError(f"{source}: the window runs past the 32-bit address space")
    if not load <= target < load + size:
        raise CaptureError(f"{source}: target 0x{target:08x} lies outside the window "
                           f"[0x{load:08x},0x{load + size:08x})")
    return Capture(load, target, pc, ra, blob[HEADER.size:], source)


def capture_bytes(capture):
    """The file the runtime writes for ``capture`` (the inverse of ``parse_capture``)."""
    return HEADER.pack(MAGIC, capture.load, len(capture.data), capture.target,
                       capture.pc, capture.ra, 0) + bytes(capture.data)


def capture_file_name(capture):
    return f"code_capture_{capture.target:08x}_{fnv1a64(capture.data):016x}{SUFFIX}"


def capture_paths(paths):
    """Every capture file ``paths`` names, a folder standing for its ``*.nkcap`` files."""
    found = []
    for given in paths:
        path = Path(given)
        if path.is_dir():
            found.extend(sorted(p for p in path.iterdir()
                                if p.suffix.lower() == SUFFIX and p.is_file()))
        elif path.is_file():
            found.append(path)
        else:
            raise CaptureError(f"{given}: no such capture file or folder")
    return found


def read_captures(paths):
    return [parse_capture(path.read_bytes(), path.name) for path in capture_paths(paths)]


def group_captures(captures):
    """The images the captures make, in a fixed order.

    Captures are taken in address order. Each one joins the first image whose window it
    overlaps and agrees with byte for byte over the overlap, which then covers both windows
    and both targets; otherwise it starts an image of its own. Two captures of the same
    address with different bytes therefore stay apart, and their translations swap at run
    time as the bytes in memory change, the way overlays that share an address do."""
    images = []
    for capture in sorted(captures, key=lambda c: (c.load, c.end, c.target, c.data, c.source)):
        for image in images:
            lo, hi = max(image.load, capture.load), min(image.end, capture.end)
            if lo >= hi:
                continue
            if image.data[lo - image.load:hi - image.load] != capture.data[lo - capture.load:
                                                                          hi - capture.load]:
                continue
            if capture.load < image.load:
                image.data[:0] = capture.data[:image.load - capture.load]
                image.load = capture.load
            if capture.end > image.end:
                image.data.extend(capture.data[image.end - capture.load:])
            break
        else:
            image = CaptureImage(capture.load, bytearray(capture.data))
            images.append(image)
        image.targets.add(capture.target)
        image.sources.append(capture.source)
    return images


def image_overlay_elf(image):
    """An ELF type 0xFFA8 overlay of ``image``: one PT_LOAD at its load address, no entry."""
    data = bytes(image.data)
    offset = _ELF_HEADER_BYTES + _PROGRAM_HEADER_BYTES
    header = (b"\x7fELF\x01\x01\x01" + bytes(9)
              + struct.pack("<HHIIIIIHHHHHH", FIXED_OVERLAY_ELF_TYPE, 8, 1, 0,
                            _ELF_HEADER_BYTES, 0, 0, _ELF_HEADER_BYTES,
                            _PROGRAM_HEADER_BYTES, 1, 40, 0, 0))
    program = struct.pack("<8I", 1, offset, image.load, image.load, len(data), len(data), 5, 4)
    return header + program + data


def translate_images(images, profile=None, cfg_gate=False, first_index=0):
    """Each image's ``codegen.RuntimeModuleTranslation``, or an int exit status."""
    import codegen
    modules = []
    for offset, image in enumerate(images):
        module = codegen.translate_code_capture(first_index + offset, image, profile=profile,
                                                cfg_gate=cfg_gate)
        if isinstance(module, int):
            return module
        modules.append(module)
    return modules


def unit_text(modules):
    """One C translation unit holding ``modules``' bodies and descriptors, registered by
    ``sr_register_code_captures()``."""
    import codegen
    lines = [
        "/* Generated by tools/code_capture.py. Do not edit by hand. */",
        '#include "recomp.h"',
        "#include <math.h>",
        "#include <string.h>",
        "#include <stdio.h>",
        "",
        "#ifndef SR_CPUSTATE_ABI_VERSION",
        '#error "generated functions require SR_CPUSTATE_ABI_VERSION"',
        f"#elif SR_CPUSTATE_ABI_VERSION != {codegen.CPU_STATE_ABI_VERSION}u",
        f'#error "generated functions require CpuState ABI version '
        f'{codegen.CPU_STATE_ABI_VERSION}u"',
        "#endif",
        "",
        "void sr_register_code_captures(void);",
    ]
    for module in modules:
        lines.append(f"extern uint32_t {module.base_symbol};")
        lines.extend(f"void {module.prefix}_{a:08x}(CpuState *s);" for a, _ in module.functions)
    lines.append("")
    for module in modules:
        lines.extend(str(text) + "\n" for text in module.texts)
    for module in modules:
        lines.extend(codegen.runtime_module_descriptor_lines(module))
    lines.append("void sr_register_code_captures(void) {")
    lines.extend(f"    sr_module_code_register(&sr_m{module.index}_code);" for module in modules)
    lines.append("}")
    return "\n".join(lines) + "\n"


def _info(ns):
    for image in group_captures(read_captures(ns.captures)):
        targets = ",".join(f"0x{t:08x}" for t in sorted(image.targets))
        print(f"{image.name} [0x{image.load:08x},0x{image.end:08x}) targets={targets} "
              f"captures={len(image.sources)}")
    return 0


def _translate(ns):
    images = group_captures(read_captures(ns.captures))
    if not images:
        sys.stderr.write("code_capture: no captures to translate\n")
        return 2
    modules = translate_images(images)
    if isinstance(modules, int):
        return modules
    Path(ns.out).write_text(unit_text(modules), encoding="ascii", newline="\n")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="list the images the captures make")
    info.add_argument("captures", nargs="+", metavar="CAPTURE")
    info.set_defaults(run=_info)
    translate = sub.add_parser("translate", help="write one C unit translating the captures")
    translate.add_argument("--out", required=True, metavar="UNIT.c")
    translate.add_argument("captures", nargs="+", metavar="CAPTURE")
    translate.set_defaults(run=_translate)
    ns = parser.parse_args(argv)
    try:
        return ns.run(ns)
    except CaptureError as exc:
        sys.stderr.write(f"code_capture: {exc}\n")
        return 2


if __name__ == "__main__":
    sys.exit(main())
