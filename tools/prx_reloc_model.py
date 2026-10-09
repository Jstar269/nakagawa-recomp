# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Load-base model of PSP module relocations.

A relocatable PSP module (ELF type 0xFFA0) is laid out wherever the guest
allocator places it at run time, so every relocated word is a function of the
module's load base.  This module enumerates the relocation sites of a module
and records, for each one, a base-independent constant from which the value the
runtime loader writes at any base follows exactly.  ``tools/codegen.py`` uses it
to translate a module once, position-independently, and the runtime uses the
same constants to verify that the image its loader produced is the image the
translation assumed (``SrModuleRelocCheck`` in ``src/rt/recomp.h``).

Project-authored from ``docs/cleanroom/PRX_LOADER_SPEC.md`` sections 3.2 and
3.5-3.6, the behaviour specification the runtime loader
(``src/rt/prx_loader.c``) implements; ``tools/test_prx_reloc_model.py`` pins
this model against that loader's output at several bases.  Where the
specification makes the relocated value depend on the load base in a way that
is not a function of one base term (a site relocated twice, or a HI16 partner
that an earlier record already relocated) the model refuses the module with
``RelocationModelError`` rather than guessing.

Site kinds and the value at base ``B`` (``C`` is ``RelocationSite.constant``):

* ``lo16``   -- low 16 bits of ``C + B``; formats A kinds 1/6, B kinds 1/5.
* ``hi16``   -- low 16 bits of ``(C + B + 0x8000) >> 16``; A kind 5, B kind 4.
* ``jump26`` -- jump field of ``region(site + B) + C + B``; A kind 4, B kinds
  3/6/7 (6 and 7 also force the primary opcode to ``j``/``jal``).
* ``word32`` -- ``C + B``; A kind 2, B kind 2.

For a fixed-address module (ELF type 2) the base is always 0 and ``C`` already
holds the segment's absolute load address.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from elf_bounds import validate_elf32_envelope

ET_REL = 1
ET_EXEC = 2
ET_SCE_PRX = 0xFFA0
PT_LOAD = 1
PRX_RELOC_A = 0x700000A0
PRX_RELOC_B = 0x700000A1
MAX_LOAD_SEGMENTS = 4

KIND_LO16 = "lo16"
KIND_HI16 = "hi16"
KIND_JUMP26 = "jump26"
KIND_WORD32 = "word32"
SITE_KINDS = (KIND_LO16, KIND_HI16, KIND_JUMP26, KIND_WORD32)

OPCODE_J = 2
OPCODE_JAL = 3


class RelocationModelError(ValueError):
    """The module's relocations cannot be modelled as a function of its base."""


def _sx16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _sign_extend(value: int, bits: int) -> int:
    if bits <= 0:
        return 0
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


@dataclass(frozen=True)
class RelocationSite:
    """One relocated word and the base-independent term of its value."""

    address: int
    kind: str
    constant: int
    opcode: int | None = None

    def field(self, base: int) -> int:
        """The relocated field the runtime loader writes at load base ``base``."""
        if self.kind == KIND_LO16:
            return (self.constant + base) & 0xFFFF
        if self.kind == KIND_HI16:
            return (((self.constant + base + 0x8000) & 0xFFFFFFFF) >> 16) & 0xFFFF
        if self.kind == KIND_JUMP26:
            region = (base + self.address) & 0xF0000000
            return (((region + self.constant + base) & 0xFFFFFFFF) >> 2) & 0x03FFFFFF
        if self.kind == KIND_WORD32:
            return (self.constant + base) & 0xFFFFFFFF
        raise AssertionError(f"unknown relocation site kind {self.kind!r}")

    def word(self, original: int, base: int) -> int:
        """The complete word at this site after relocation at ``base``."""
        field = self.field(base)
        if self.kind in (KIND_LO16, KIND_HI16):
            return (original & 0xFFFF0000) | field
        if self.kind == KIND_JUMP26:
            high = original & 0xFC000000
            if self.opcode is not None:
                high = (self.opcode & 0x3F) << 26
            return high | field
        return field

    def target(self, base: int) -> int:
        """The jump target a ``jump26`` site transfers to at ``base``."""
        if self.kind != KIND_JUMP26:
            raise AssertionError("only a jump26 site has a jump target")
        region = (base + self.address) & 0xF0000000
        return region | (self.field(base) << 2)


@dataclass(frozen=True)
class LoadSegment:
    vaddr: int
    memsz: int
    data: bytes
    align: int


@dataclass(frozen=True)
class RelocationModel:
    """Every relocation site of one module, keyed by its base-0 address."""

    relocatable: bool
    segments: tuple[LoadSegment, ...]
    sites: dict[int, RelocationSite]

    @property
    def load_low(self) -> int:
        return min(segment.vaddr for segment in self.segments)

    @property
    def load_high(self) -> int:
        return max(segment.vaddr + segment.memsz for segment in self.segments)

    @property
    def alignment(self) -> int:
        return max(max(segment.align, 1) for segment in self.segments)

    def site(self, address: int) -> RelocationSite | None:
        return self.sites.get(address)

    def segment_image(self, index: int, base: int) -> bytes:
        """Segment ``index`` exactly as the runtime loader lays it out at ``base``."""
        if not self.relocatable and base != 0:
            raise RelocationModelError("a fixed-address module only loads at base 0")
        segment = self.segments[index]
        image = bytearray(segment.memsz)
        image[: len(segment.data)] = segment.data
        originals = bytes(image)
        for address, site in sorted(self.sites.items()):
            offset = address - segment.vaddr
            if offset < 0 or offset + 4 > segment.memsz:
                continue
            original = struct.unpack_from("<I", originals, offset)[0]
            struct.pack_into("<I", image, offset, site.word(original, base))
        return bytes(image)


class _Image:
    """The words a relocation stream reads, tracked as the loader would see them."""

    def __init__(self, segments: list[LoadSegment], relocatable: bool) -> None:
        self.segments = segments
        self.relocatable = relocatable
        self.sites: dict[int, RelocationSite] = {}

    def site_address(self, segment_index: int, offset: int) -> int:
        if segment_index >= len(self.segments):
            raise RelocationModelError("relocation offset segment is out of range")
        segment = self.segments[segment_index]
        if offset < 0 or offset + 4 > segment.memsz:
            raise RelocationModelError("relocation site lies outside its segment")
        return segment.vaddr + offset

    def original_word(self, address: int) -> int:
        for segment in self.segments:
            offset = address - segment.vaddr
            if 0 <= offset and offset + 4 <= segment.memsz:
                if offset + 4 <= len(segment.data):
                    return struct.unpack_from("<I", segment.data, offset)[0]
                raw = segment.data[offset:offset + 4] if offset < len(segment.data) else b""
                return int.from_bytes(raw + b"\0" * (4 - len(raw)), "little")
        raise RelocationModelError("relocation site lies outside the module image")

    def unrelocated_word(self, address: int) -> int:
        """A word whose value does not yet depend on the base, or refuse.

        Any earlier site whose four bytes overlap this word makes the word's
        current value depend on the base already, so it is refused too."""
        for neighbour in range(address - 3, address + 4):
            if neighbour in self.sites:
                raise RelocationModelError(
                    f"relocation reads site 0x{address:08x} after an earlier record "
                    "relocated it; its value would not be a function of one base term"
                )
        return self.original_word(address)

    def target_load_address(self, segment_index: int) -> int:
        if segment_index >= len(self.segments):
            raise RelocationModelError("relocation address segment is out of range")
        return self.segments[segment_index].vaddr

    def record(self, address: int, kind: str, constant: int, opcode: int | None = None) -> None:
        self.unrelocated_word(address)
        self.sites[address] = RelocationSite(address, kind, constant & 0xFFFFFFFF, opcode)

    def apply(self, address: int, kind_name: str | None, target_segment: int,
              opcode: int | None = None) -> None:
        if kind_name is None:
            return
        word = self.unrelocated_word(address)
        load = self.target_load_address(target_segment)
        if kind_name == KIND_LO16:
            self.record(address, KIND_LO16, _sx16(word) + load)
        elif kind_name == KIND_WORD32:
            self.record(address, KIND_WORD32, word + load)
        elif kind_name == KIND_JUMP26:
            self.record(address, KIND_JUMP26, ((word & 0x03FFFFFF) << 2 & 0x0FFFFFFF) + load,
                        opcode)
        else:
            raise AssertionError(kind_name)


_FORMAT_A_KINDS = {0: None, 1: KIND_LO16, 2: KIND_WORD32, 4: KIND_JUMP26, 6: KIND_LO16, 8: None}


def _apply_format_a(image: _Image, table: bytes) -> None:
    if len(table) % 8:
        raise RelocationModelError("format A relocation table is not a multiple of 8 bytes")
    records = [struct.unpack_from("<II", table, index) for index in range(0, len(table), 8)]
    nseg = len(image.segments)

    def fields(info: int) -> tuple[int, int, int]:
        kind, offset_segment, address_segment = info & 0xFF, (info >> 8) & 0xFF, (info >> 16) & 0xFF
        if offset_segment >= nseg or address_segment >= nseg:
            raise RelocationModelError("format A segment index is out of range")
        return kind, offset_segment, address_segment

    index = 0
    while index < len(records):
        offset, info = records[index]
        kind, offset_segment, address_segment = fields(info)
        if kind == 5:
            run_end = index
            while run_end < len(records) and records[run_end][1] & 0xFF == 5:
                fields(records[run_end][1])
                run_end += 1
            if run_end >= len(records):
                raise RelocationModelError("format A HI16 run has no partner record")
            partner_offset, partner_info = records[run_end]
            partner_kind, partner_offset_segment, partner_address_segment = fields(partner_info)
            partner_address = image.site_address(partner_offset_segment, partner_offset)
            partner_low = _sx16(image.unrelocated_word(partner_address))
            for hi_offset, hi_info in records[index:run_end]:
                _, hi_offset_segment, hi_address_segment = fields(hi_info)
                hi_address = image.site_address(hi_offset_segment, hi_offset)
                hi_word = image.unrelocated_word(hi_address)
                image.record(
                    hi_address, KIND_HI16,
                    ((hi_word & 0xFFFF) << 16) + partner_low
                    + image.target_load_address(hi_address_segment),
                )
            _apply_format_a_single(image, partner_address, partner_kind, partner_address_segment)
            index = run_end + 1
            continue
        _apply_format_a_single(image, image.site_address(offset_segment, offset), kind,
                               address_segment)
        index += 1


def _apply_format_a_single(image: _Image, address: int, kind: int, address_segment: int) -> None:
    if kind == 5:
        raise RelocationModelError("format A HI16 run has no partner record")
    if kind == 7:
        raise RelocationModelError("format A kind 7 (GPREL16) is refused by the runtime loader")
    if kind not in _FORMAT_A_KINDS:
        raise RelocationModelError(f"format A relocation kind {kind} is not supported")
    image.apply(address, _FORMAT_A_KINDS[kind], address_segment)


def _apply_format_b(image: _Image, stream: bytes) -> None:
    length = len(stream)
    if length < 4:
        raise RelocationModelError("format B header is truncated")
    if stream[0] != 0 or stream[1] != 0:
        raise RelocationModelError("format B header bytes 0-1 are not zero")
    flag_bits, kind_bits = stream[2], stream[3]
    segment_bits = 2 if len(image.segments) >= 3 else 1
    if not (1 <= flag_bits <= 8 and 1 <= kind_bits <= 8):
        raise RelocationModelError("format B flag/kind field widths are invalid")
    if flag_bits + kind_bits + segment_bits > 16:
        raise RelocationModelError("format B field widths exceed 16 bits")
    if length < 5:
        raise RelocationModelError("format B flag table is truncated")
    flag_count = stream[4]
    if flag_count < 1 or 4 + flag_count > length:
        raise RelocationModelError("format B flag table is truncated")
    kind_table = 4 + flag_count
    if kind_table >= length:
        raise RelocationModelError("format B kind table is truncated")
    kind_count = stream[kind_table]
    if kind_count < 1 or kind_table + kind_count > length:
        raise RelocationModelError("format B kind table is truncated")
    cursor = kind_table + kind_count
    displacement_bits = 16 - flag_bits - segment_bits - kind_bits

    def take(size: int, label: str) -> int:
        nonlocal cursor
        if cursor + size > length:
            raise RelocationModelError(f"format B {label} is truncated")
        value = int.from_bytes(stream[cursor:cursor + size], "little")
        cursor += size
        return value

    current_segment = 0
    current_offset = 0
    previous_kind = -1
    previous_addend = 0
    while cursor < length:
        command = take(2, "command")
        flag_index = command & ((1 << flag_bits) - 1)
        segment_index = (command >> flag_bits) & ((1 << segment_bits) - 1)
        kind_index = (command >> (flag_bits + segment_bits)) & ((1 << kind_bits) - 1)
        raw_displacement = command >> (flag_bits + segment_bits + kind_bits)
        displacement = _sign_extend(raw_displacement, displacement_bits)
        if flag_index == 0 or flag_index >= flag_count:
            raise RelocationModelError("format B flag index is out of range")
        flag = stream[4 + flag_index]
        if segment_index >= len(image.segments):
            raise RelocationModelError("format B segment index is out of range")
        if not flag & 1:
            current_segment = segment_index
            offset_form = flag & 6
            if offset_form == 0:
                current_offset = command >> (flag_bits + segment_bits)
            elif offset_form == 4:
                current_offset = take(4, "extension word")
            else:
                raise RelocationModelError("format B set-offset form is invalid")
            continue
        if kind_index == 0 or kind_index >= kind_count:
            raise RelocationModelError("format B kind index is out of range")
        kind = stream[kind_table + kind_index]
        offset_form = flag & 6
        if offset_form == 0:
            current_offset = (current_offset + displacement) & 0xFFFFFFFF
        elif offset_form == 2:
            extension = take(2, "extension word")
            full = (((displacement & 0xFFFF) << 16) | extension) & 0xFFFFFFFF
            current_offset = (current_offset + _sign_extend(full, 32)) & 0xFFFFFFFF
        elif offset_form == 4:
            current_offset = take(4, "extension word")
        else:
            raise RelocationModelError("format B relocate-offset form is invalid")
        addend_form = (flag >> 3) & 7
        if addend_form == 0:
            addend = 0
        elif addend_form == 1:
            addend = previous_addend if previous_kind == 4 else 0
        elif addend_form == 2:
            addend = _sx16(take(2, "addend word"))
        else:
            raise RelocationModelError("format B addend form is invalid")
        address = image.site_address(current_segment, current_offset)
        if kind == 0:
            pass
        elif kind in (1, 5):
            image.apply(address, KIND_LO16, segment_index)
        elif kind == 2:
            image.apply(address, KIND_WORD32, segment_index)
        elif kind == 3:
            image.apply(address, KIND_JUMP26, segment_index)
        elif kind == 4:
            word = image.unrelocated_word(address)
            image.record(
                address, KIND_HI16,
                ((word & 0xFFFF) << 16) + _sx16(addend) + image.target_load_address(segment_index),
            )
        elif kind == 6:
            image.apply(address, KIND_JUMP26, segment_index, OPCODE_J)
        elif kind == 7:
            image.apply(address, KIND_JUMP26, segment_index, OPCODE_JAL)
        else:
            raise RelocationModelError(f"format B relocation kind {kind} is not supported")
        previous_kind = kind
        previous_addend = addend


def model_relocations(data: bytes, label: str = "<module>") -> RelocationModel:
    """Enumerate the relocation sites of one decrypted PSP module image."""
    try:
        envelope = validate_elf32_envelope(bytes(data), label)
    except ValueError as exc:
        raise RelocationModelError(str(exc)) from exc
    if envelope["machine"] != 8:
        raise RelocationModelError(f"{label}: not a MIPS module")
    e_type = envelope["e_type"]
    if e_type not in (ET_SCE_PRX, ET_REL, ET_EXEC):
        raise RelocationModelError(f"{label}: unsupported ELF type 0x{e_type:x}")
    loads = [ph for ph in envelope["phdrs"] if ph["type"] == PT_LOAD]
    if not loads:
        raise RelocationModelError(f"{label}: no loadable segment")
    if len(loads) > MAX_LOAD_SEGMENTS:
        raise RelocationModelError(f"{label}: more than {MAX_LOAD_SEGMENTS} loadable segments")
    segments = [
        LoadSegment(ph["vaddr"], ph["memsz"], bytes(data[ph["off"]:ph["off"] + ph["filesz"]]),
                    ph["align"])
        for ph in loads
    ]
    for segment in segments:
        if segment.vaddr + segment.memsz > 0x100000000:
            raise RelocationModelError(f"{label}: segment exceeds the 32-bit address space")
    image = _Image(segments, e_type in (ET_SCE_PRX, ET_REL))
    has_program_format_a = False
    for ph in envelope["phdrs"]:
        if ph["type"] == PRX_RELOC_A:
            has_program_format_a = True
            _apply_format_a(image, bytes(data[ph["off"]:ph["off"] + ph["filesz"]]))
        elif ph["type"] == PRX_RELOC_B:
            _apply_format_b(image, bytes(data[ph["off"]:ph["off"] + ph["filesz"]]))
    if not has_program_format_a:
        for sh in envelope["shdrs"]:
            if sh["typ"] == PRX_RELOC_A:
                _apply_format_a(image, bytes(data[sh["off"]:sh["off"] + sh["size"]]))
    if not image.relocatable and image.sites:
        raise RelocationModelError(
            f"{label}: a fixed-address module carries relocation records; its image "
            "would not match its link-time words"
        )
    return RelocationModel(image.relocatable, tuple(segments), dict(image.sites))
