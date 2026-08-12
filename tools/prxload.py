# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# PSP PRX loader: rebase a relocatable PRX to a load base and apply its type-A relocations,
# producing a flat memory image with concrete addresses (the form the analyzer/codegen need).
# Ports PPSSPP's ElfReader::LoadRelocations (type-A, section type 0x700000A0).
#
# Usage: prxload.py <prx-elf> <base-hex> [--psp-header EBOOT.BIN]
#                   [--out image.bin] [--verify pc=word ...]

import struct
import sys

from elf_bounds import (
    MAX_ELF_IMAGE_BYTES,
    checked_span,
    image_extent,
    validate_elf32_envelope,
)

R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_26 = 0, 1, 2, 4
R_MIPS_HI16, R_MIPS_LO16, R_MIPS_GPREL16 = 5, 6, 7
SHT_PRX_RELOC = 0x700000A0


def read_psp_segment_sizes(path, expected_segments):
    """Read the authoritative in-memory segment sizes from a ~PSP header.

    Some decrypted/stripped PSP ELFs preserve only PT_LOAD file bytes and set
    ``p_memsz == p_filesz``, losing the module's trailing BSS extent.  The
    original encrypted module header retains both the total segment sizes and
    the aggregate BSS size.  Keeping that metadata in the flat image prevents
    the user partition from being placed on top of omitted static storage.
    """
    with open(path, "rb") as f:
        header = f.read(0x80)
    if len(header) < 0x64 or header[:4] != b"~PSP":
        raise ValueError(f"{path}: not a valid ~PSP module header")

    segment_count = header[0x27]
    if segment_count == 0 or segment_count > 4:
        raise ValueError(f"{path}: invalid PSP segment count {segment_count}")
    if segment_count != expected_segments:
        raise ValueError(
            f"{path}: PSP header has {segment_count} load segments, "
            f"ELF has {expected_segments}"
        )

    bss_size = struct.unpack_from("<I", header, 0x38)[0]
    segment_sizes = list(struct.unpack_from("<4I", header, 0x54))[:segment_count]
    return segment_sizes, bss_size


class Prx:
    def __init__(self, path, base, psp_header=None):
        if not isinstance(base, int) or base < 0 or base > 0xFFFFFFFF:
            raise ValueError(f"{path}: load base is outside the 32-bit address space")
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        envelope = validate_elf32_envelope(d, path)
        self.entry = envelope["entry"]
        self.phoff = envelope["phoff"]
        self.shoff = envelope["shoff"]
        self.phentsize = envelope["phentsize"]
        self.phnum = envelope["phnum"]
        self.shentsize = envelope["shentsize"]
        self.shnum = envelope["shnum"]
        self.shstrndx = envelope["shstrndx"]
        self.base = base

        self.segments = [
            dict(
                type=p["type"],
                off=p["off"],
                vaddr=p["vaddr"],
                filesz=p["filesz"],
                memsz=p["memsz"],
                idx=p["idx"],
            )
            for p in envelope["phdrs"]
        ]
        loads = [s for s in self.segments if s["type"] == 1]
        if not loads:
            raise ValueError(f"{path}: ELF has no PT_LOAD segments")

        self.psp_bss_size = 0
        self.psp_segment_sizes = None
        if psp_header is not None:
            segment_sizes, self.psp_bss_size = read_psp_segment_sizes(
                psp_header, len(loads)
            )
            declared_extra = 0
            for segment, declared_size in zip(loads, segment_sizes):
                if declared_size < segment["filesz"]:
                    raise ValueError(
                        f"{psp_header}: segment memory size 0x{declared_size:x} "
                        f"is smaller than ELF file size 0x{segment['filesz']:x}"
                    )
                declared_extra += declared_size - segment["filesz"]
                segment["memsz"] = max(segment["memsz"], declared_size)
            if declared_extra < self.psp_bss_size:
                raise ValueError(
                    f"{psp_header}: segment sizes provide only 0x{declared_extra:x} "
                    f"bytes beyond filesz for declared BSS 0x{self.psp_bss_size:x}"
                )
            self.psp_segment_sizes = segment_sizes
        # segVAddr[i] = where program-header segment i is loaded. PSP relocs index program
        # segments; only PT_LOAD segments carry an image, indexed in header order.
        self.seg_vaddr = [base + s["vaddr"] for s in loads]

        # Flat image covering all loaded segments.  The extent check happens
        # before bytearray allocation so forged BSS cannot request unbounded
        # host memory or wrap the image offset.
        extents = [
            image_extent(base, s["vaddr"], s["memsz"], f"{path}: PT_LOAD {s['idx']}")
            for s in loads
        ]
        end = max(end for _, end in extents)
        self.lo = base
        image_size = end - base
        if image_size > MAX_ELF_IMAGE_BYTES:
            raise ValueError(f"{path}: flat image exceeds the 256 MiB bound")
        self.mem = bytearray(image_size)
        for s in loads:
            dst = (base + s["vaddr"]) - self.lo
            checked_span(len(d), s["off"], s["filesz"], f"{path}: PT_LOAD data")
            payload = d[s["off"] : s["off"] + s["filesz"]]
            if len(payload) != s["filesz"]:
                raise ValueError(f"{path}: PT_LOAD data is truncated")
            checked_span(len(self.mem), dst, s["filesz"], f"{path}: PT_LOAD image")
            self.mem[dst : dst + s["filesz"]] = payload

        self.sections = []
        if self.shnum > 0 and self.shentsize > 0:
            for section in envelope["shdrs"]:
                typ, off, size = section["typ"], section["off"], section["size"]
                seg_idx = -1
                for seg in self.segments:
                    if seg["off"] <= off < seg["off"] + seg["filesz"]:
                        seg_idx = seg["idx"]
                        break
                self.sections.append(dict(typ=typ, off=off, size=size, seg_idx=seg_idx))
        else:
            for seg in self.segments:
                if seg["type"] in (0x700000a0, 0x700000a1):
                    self.sections.append(dict(typ=seg["type"], off=seg["off"], size=seg["filesz"], seg_idx=seg["idx"]))

    def r32(self, addr):
        o = addr - self.lo
        checked_span(len(self.mem), o, 4, "relocation read")
        return struct.unpack("<I", self.mem[o:o + 4])[0]

    def w32(self, addr, val):
        o = addr - self.lo
        checked_span(len(self.mem), o, 4, "relocation write")
        self.mem[o:o + 4] = struct.pack("<I", val & 0xFFFFFFFF)

    def relocate(self):
        d = self.data
        total = 0
        for sec in self.sections:
            if sec["typ"] == 0x700000a0:
                if sec["size"] % 8:
                    raise ValueError("type-A relocation section is not an 8-byte table")
                checked_span(len(d), sec["off"], sec["size"], "type-A relocation table")
                n = sec["size"] // 8
                rels = [struct.unpack("<II", d[sec["off"] + r * 8:sec["off"] + r * 8 + 8]) for r in range(n)]
                self._apply(rels)
                total += n
            elif sec["typ"] == 0x700000a1:
                n = self._apply_packed(sec)
                total += n
        return total

    def _apply_packed(self, sec):
        d = self.data
        rel_seg_idx = sec["seg_idx"]
        buf_offset = sec["off"]
        filesz = sec["size"]

        if rel_seg_idx < 0 or rel_seg_idx >= len(self.segments):
            raise ValueError("packed relocation section has no valid program-header index")
        checked_span(len(d), buf_offset, filesz, "packed relocation table")
        if filesz < 4:
            raise ValueError("packed relocation table is truncated")

        flag_bits = d[buf_offset + 2]
        type_bits = d[buf_offset + 3]

        seg_bits = 1
        while (1 << seg_bits) <= rel_seg_idx:
            seg_bits += 1

        if not (1 <= flag_bits <= 8 and 1 <= type_bits <= 8) or flag_bits + type_bits + seg_bits > 16:
            raise ValueError("packed relocation bit widths are invalid")

        o = buf_offset + 4
        checked_span(buf_offset + filesz, o, 1, "packed relocation flag table size")
        flag_table_size = d[o]
        o += flag_table_size

        checked_span(buf_offset + filesz, o, 1, "packed relocation type table size")
        type_table_size = d[o]
        o += type_table_size

        if flag_table_size == 0 or type_table_size == 0:
            raise ValueError("packed relocation tables are empty")
        checked_span(buf_offset + filesz, buf_offset + 4, flag_table_size + type_table_size, "packed relocation tables")

        end_offset = buf_offset + filesz
        rel_base = 0
        last_type = -1
        lo16 = 0
        off_seg = 0
        rcount = 0

        nseg = len(self.seg_vaddr)

        while o < end_offset:
            checked_span(end_offset, o, 2, "packed relocation command")
            cmd = struct.unpack("<H", d[o:o + 2])[0]
            o += 2

            flag_idx = (cmd << (16 - flag_bits)) & 0xFFFF
            flag_idx = flag_idx >> (16 - flag_bits)
            if flag_idx >= flag_table_size:
                raise ValueError("packed relocation flag index is out of range")
            flag = d[buf_offset + 4 + flag_idx]

            seg_val = (cmd << (16 - seg_bits - flag_bits)) & 0xFFFF
            seg_val = seg_val >> (16 - seg_bits)

            type_idx = (cmd << (16 - type_bits - seg_bits - flag_bits)) & 0xFFFF
            type_idx = type_idx >> (16 - type_bits)
            if type_idx >= type_table_size:
                raise ValueError("packed relocation type index is out of range")
            rtype = d[buf_offset + 4 + flag_table_size + type_idx]

            if (flag & 0x01) == 0:
                off_seg = seg_val
                if (flag & 0x06) == 0:
                    rel_base = cmd >> (seg_bits + flag_bits)
                elif (flag & 0x06) == 4:
                    checked_span(end_offset, o, 4, "packed relocation base")
                    rel_base = struct.unpack("<I", d[o:o+4])[0]
                    o += 4
                else:
                    rel_base = 0
            else:
                addr_seg = seg_val
                # The packed PSP stream encodes all program-header segment
                # slots, including non-load relocation metadata.  Those slots
                # deliberately contribute no guest address; keep the mapping
                # bounded instead of indexing past the PT_LOAD table.
                relocate_to = self.seg_vaddr[addr_seg] if addr_seg < nseg else 0

                if (flag & 0x06) == 0x00:
                    rel_offset = cmd
                    if cmd & 0x8000:
                        rel_offset |= 0xFFFF0000
                        rel_offset >>= (type_bits + seg_bits + flag_bits)
                        rel_offset |= 0xFFFF0000
                    else:
                        rel_offset >>= (type_bits + seg_bits + flag_bits)
                    rel_base += rel_offset
                elif (flag & 0x06) == 0x02:
                    rel_offset = cmd
                    if cmd & 0x8000:
                        rel_offset |= 0xFFFF0000
                    rel_offset >>= (type_bits + seg_bits + flag_bits)
                    checked_span(end_offset, o, 2, "packed relocation low addend")
                    lo_part = struct.unpack("<H", d[o:o+2])[0]
                    o += 2
                    rel_offset = (rel_offset << 16) | lo_part
                    if rel_offset & 0x80000000:
                        rel_offset -= 0x100000000
                    rel_base += rel_offset
                elif (flag & 0x06) == 0x04:
                    checked_span(end_offset, o, 4, "packed relocation absolute base")
                    rel_base = struct.unpack("<I", d[o:o+4])[0]
                    o += 4

                off_seg_vaddr = self.seg_vaddr[off_seg] if off_seg < nseg else 0
                rel_offset_addr = (rel_base + off_seg_vaddr) & 0xFFFFFFFF

                if (flag & 0x38) == 0x00:
                    lo16 = 0
                elif (flag & 0x38) == 0x08:
                    if last_type != 0x04:
                        lo16 = 0
                elif (flag & 0x38) == 0x10:
                    checked_span(end_offset, o, 2, "packed relocation low half")
                    lo16 = struct.unpack("<H", d[o:o+2])[0]
                    if lo16 & 0x8000:
                        lo16 |= 0xFFFF0000
                        lo16 -= 0x100000000
                    o += 2

                op = self.r32(rel_offset_addr)
                last_type = rtype

                if rtype == 0:
                    continue
                elif rtype == 2:
                    op = (op + relocate_to) & 0xFFFFFFFF
                elif rtype in (3, 6, 7):
                    op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (relocate_to >> 2)) & 0x03FFFFFF)
                    if rtype == 6:
                        op = (op & ~0xFC000000) | 0x08000000
                    elif rtype == 7:
                        op = (op & ~0xFC000000) | 0x0C000000
                elif rtype == 4:
                    addr = (((op & 0xFFFF) << 16) + lo16 + relocate_to) & 0xFFFFFFFF
                    if addr & 0x8000:
                        addr += 0x00010000
                    op = (op & 0xFFFF0000) | ((addr >> 16) & 0xFFFF)
                elif rtype in (1, 5):
                    op = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocate_to) & 0xFFFF)

                self.w32(rel_offset_addr, op)
                rcount += 1
        return rcount

    def _apply(self, rels):
        nseg = len(self.seg_vaddr)
        n = len(rels)
        for r in range(n):
            offset, info = rels[r]
            rtype = info & 0xF
            ofs_seg = (info >> 8) & 0xFF
            addr_seg = (info >> 16) & 0xFF
            if ofs_seg >= nseg or addr_seg >= nseg:
                raise ValueError("type-A relocation segment index is out of range")
            addr = offset + self.seg_vaddr[ofs_seg]
            relocate_to = self.seg_vaddr[addr_seg]
            op = self.r32(addr)

            if rtype == R_MIPS_32:
                op = (op + relocate_to) & 0xFFFFFFFF
            elif rtype == R_MIPS_26:
                op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (relocate_to >> 2)) & 0x03FFFFFF)
            elif rtype == R_MIPS_HI16:
                # A HI16 relocation's addend comes from the LOW half at its paired LO16
                # (or the PSP's R_MIPS_16 alternative) record, which the ELF convention
                # places somewhere after this HI16 in table order (possibly after more
                # HI16es sharing the same pairing, per the "multiple HI16 share one LO16"
                # rule below). Any OTHER relocation type sitting between them (R_MIPS_32,
                # R_MIPS_26, R_MIPS_GPREL16, ...) is unrelated and must be skipped, not
                # mistaken for the low half -- treating it as the pair partner reads a
                # different instruction's bits as the low addend and corrupts this HI16.
                cur = (op & 0xFFFF) << 16
                for t in range(r + 1, n):
                    t_type = rels[t][1] & 0xF
                    if t_type == R_MIPS_HI16:
                        continue
                    if t_type not in (R_MIPS_LO16, R_MIPS_16):
                        continue
                    lo_op = self.r32(rels[t][0] + self.seg_vaddr[(rels[t][1] >> 8) & 0xFF])
                    s16 = (lo_op & 0xFFFF) - 0x10000 if (lo_op & 0x8000) else (lo_op & 0xFFFF)
                    cur = (cur + s16 + relocate_to) & 0xFFFFFFFF
                    hi = ((cur >> 16) + (1 if (cur & 0x8000) else 0)) & 0xFFFF
                    op = (op & 0xFFFF0000) | hi
                    break
                else:
                    # No paired LO16/R_MIPS_16 found for this HI16 (malformed/unpaired
                    # input) -- apply the segment delta alone with a zero low addend
                    # rather than silently using an unrelated relocation's bits.
                    cur = (cur + relocate_to) & 0xFFFFFFFF
                    hi = ((cur >> 16) + (1 if (cur & 0x8000) else 0)) & 0xFFFF
                    op = (op & 0xFFFF0000) | hi
            elif rtype == R_MIPS_LO16:
                op = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocate_to) & 0xFFFF)
            elif rtype == R_MIPS_16:
                op = (op & 0xFFFF0000) | ((((op & 0xFFFF)) + relocate_to) & 0xFFFF)
            # R_MIPS_GPREL16 / R_MIPS_NONE: nothing.
            self.w32(addr, op)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(
            "usage: prxload.py <prx-elf> <base-hex> "
            "[--psp-header=EBOOT.BIN] [--out=image.bin] [pc=word ...]\n"
        )
        return 2
    base = int(argv[2], 16)
    psp_header = None
    for arg in argv[3:]:
        if arg.startswith("--psp-header="):
            psp_header = arg.split("=", 1)[1]
    prx = Prx(argv[1], base, psp_header=psp_header)
    n = prx.relocate()
    bss_note = (
        f" psp_bss=0x{prx.psp_bss_size:x}" if prx.psp_segment_sizes is not None else ""
    )
    print(
        f"loaded base=0x{base:08x} entry=0x{base + prx.entry:08x} "
        f"image_size=0x{len(prx.mem):x}{bss_note} relocations applied={n}"
    )
    rc = 0
    for a in argv[3:]:
        if a.startswith("--psp-header="):
            continue
        if a.startswith("--out="):
            open(a.split("=", 1)[1], "wb").write(prx.mem)
            print("wrote image:", a.split("=", 1)[1])
        elif "=" in a:
            pc_s, word_s = a.split("=")
            pc, want = int(pc_s, 16), int(word_s, 16)
            got = prx.r32(pc)
            ok = got == want
            print(f"verify 0x{pc:08x}: got 0x{got:08x} want 0x{want:08x} {'OK' if ok else 'FAIL'}")
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
