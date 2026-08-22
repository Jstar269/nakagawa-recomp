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

import hashlib
import struct
import sys
from dataclasses import dataclass

from elf_bounds import (
    MAX_ELF_FILE_BYTES,
    MAX_ELF_IMAGE_BYTES,
    checked_span,
    image_extent,
    validate_elf32_envelope,
)

R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_REL32, R_MIPS_26 = 0, 1, 2, 3, 4
R_MIPS_HI16, R_MIPS_LO16, R_MIPS_GPREL16, R_MIPS_LITERAL = 5, 6, 7, 8
SHT_PRX_RELOC = 0x700000A0
SHT_PRX_RELOC_PACKED = 0x700000A1
PROGRAM_IMAGE_SCHEMA_VERSION = 1
UINT32_END = 0x1_0000_0000
MAX_PROGRAM_IMAGE_TABLE_BYTES = 16 * 1024 * 1024
MAX_PROGRAM_IMAGE_STRING_BYTES = 1024

_TYPE_A_SUPPORTED_TYPES = frozenset(
    (R_MIPS_16, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16)
)
_TYPE_A_NOOP_TYPES = frozenset((R_MIPS_NONE, R_MIPS_GPREL16))
_TYPE_A_SPECIAL_TYPES = frozenset((R_MIPS_LITERAL,))
# R_MIPS_REL32 is part of the MIPS relocation vocabulary and has a distinct
# packed-stream handler, but the Type-A PSP section path does not support it.
_TYPE_A_KNOWN_UNSUPPORTED_TYPES = frozenset((R_MIPS_REL32,))
_TYPE_A_RECOGNIZED_TYPES = (
    _TYPE_A_SUPPORTED_TYPES
    | _TYPE_A_NOOP_TYPES
    | _TYPE_A_SPECIAL_TYPES
    | _TYPE_A_KNOWN_UNSUPPORTED_TYPES
)


@dataclass(frozen=True)
class ProgramImageSpan:
    """An immutable half-open guest or file span."""

    start: int
    end: int

    def __post_init__(self):
        if type(self.start) is not int or type(self.end) is not int:
            raise TypeError("program-image spans require integer endpoints")
        if self.start < 0 or self.end < self.start or self.end > UINT32_END:
            raise ValueError("program-image span is outside the 32-bit address space")

    @property
    def size(self):
        return self.end - self.start

    def as_dict(self):
        return {"start": self.start, "end": self.end}


@dataclass(frozen=True)
class ProgramImageFinding:
    """Structured validation or informational finding attached to an image."""

    code: str
    severity: str
    path: str
    message: str

    def as_dict(self):
        return {
            "code": self.code,
            "message": self.message,
            "path": self.path,
            "severity": self.severity,
        }


@dataclass(frozen=True)
class ProgramImageSegment:
    index: int
    source_offset: int
    file_size: int
    memory_size: int
    virtual_address: int
    guest_start: int
    guest_end: int
    permissions: str
    alignment: int
    file_span: ProgramImageSpan
    memory_span: ProgramImageSpan
    zero_fill: ProgramImageSpan | None

    def as_dict(self):
        return {
            "alignment": self.alignment,
            "file_size": self.file_size,
            "file_span": self.file_span.as_dict(),
            "guest_end": self.guest_end,
            "guest_start": self.guest_start,
            "index": self.index,
            "memory_size": self.memory_size,
            "memory_span": self.memory_span.as_dict(),
            "permissions": self.permissions,
            "source_offset": self.source_offset,
            "virtual_address": self.virtual_address,
            "zero_fill": self.zero_fill.as_dict() if self.zero_fill else None,
        }


@dataclass(frozen=True)
class ProgramImageSection:
    index: int
    name: str
    section_type: int
    flags: int
    address: int
    size: int
    source_offset: int
    entry_size: int

    def as_dict(self):
        return {
            "address": self.address,
            "entry_size": self.entry_size,
            "flags": self.flags,
            "index": self.index,
            "name": self.name,
            "section_type": self.section_type,
            "size": self.size,
            "source_offset": self.source_offset,
        }


@dataclass(frozen=True)
class ProgramImageImport:
    library: str
    address: int
    function_count: int
    nid_table: int
    stub_table: int
    nids: tuple[int, ...]

    def as_dict(self):
        return {
            "address": self.address,
            "function_count": self.function_count,
            "library": self.library,
            "nid_table": self.nid_table,
            "nids": list(self.nids),
            "stub_table": self.stub_table,
        }


@dataclass(frozen=True)
class ProgramImageExport:
    library: str
    address: int
    function_count: int
    variable_count: int
    function_table: int
    variable_table: int
    functions: tuple[int, ...]
    variables: tuple[int, ...]

    def as_dict(self):
        return {
            "address": self.address,
            "function_count": self.function_count,
            "function_table": self.function_table,
            "functions": list(self.functions),
            "library": self.library,
            "variable_count": self.variable_count,
            "variable_table": self.variable_table,
            "variables": list(self.variables),
        }


@dataclass(frozen=True)
class ProgramImageRelocation:
    section_index: int
    source_offset: int
    relocation_type: int
    offset: int
    info: int
    target_address: int | None

    def as_dict(self):
        return {
            "info": self.info,
            "offset": self.offset,
            "relocation_type": self.relocation_type,
            "section_index": self.section_index,
            "source_offset": self.source_offset,
            "target_address": self.target_address,
        }


@dataclass(frozen=True)
class ProgramImageModule:
    name: str
    attributes: int
    version: int
    gp: int
    export_start: int
    export_end: int
    import_start: int
    import_end: int

    def as_dict(self):
        return {
            "attributes": self.attributes,
            "export": {"end": self.export_end, "start": self.export_start},
            "gp": self.gp,
            "import": {"end": self.import_end, "start": self.import_start},
            "name": self.name,
            "version": self.version,
        }


class ProgramImageValidationError(ValueError):
    """Raised when a hostile input cannot form a structurally valid ProgramImage."""

    def __init__(self, findings):
        self.findings = tuple(findings)
        detail = "; ".join(f"{item.code}: {item.message}" for item in self.findings)
        super().__init__(detail or "program image validation failed")


@dataclass(frozen=True)
class ProgramImage:
    """Versioned, immutable truth shared by future analysis backends.

    ``source_bytes`` and ``flat_bytes`` are ``bytes`` rather than bytearrays.  A
    caller can therefore inspect or compare an image but cannot mutate the bytes
    that validation and downstream reports describe.  Relocation application is
    intentionally outside this object; the legacy :class:`Prx` remains the
    authoritative HST rebase/relocation path until a later migration proves the
    read-only representation against it.
    """

    schema_version: int
    source_name: str
    source_sha256: str
    source_size: int
    elf_type: int
    machine: int
    load_base: int
    entry_point: int
    segments: tuple[ProgramImageSegment, ...]
    executable_intervals: tuple[ProgramImageSpan, ...]
    sections: tuple[ProgramImageSection, ...]
    imports: tuple[ProgramImageImport, ...]
    exports: tuple[ProgramImageExport, ...]
    relocations: tuple[ProgramImageRelocation, ...]
    module: ProgramImageModule | None
    findings: tuple[ProgramImageFinding, ...]
    image_start: int
    image_end: int
    source_bytes: bytes
    flat_bytes: bytes

    def __post_init__(self):
        if self.schema_version != PROGRAM_IMAGE_SCHEMA_VERSION:
            raise ValueError("unsupported ProgramImage schema version")
        if not isinstance(self.source_bytes, bytes) or not isinstance(self.flat_bytes, bytes):
            raise TypeError("ProgramImage payloads must be immutable bytes")
        if not isinstance(self.segments, tuple) or not isinstance(self.executable_intervals, tuple):
            raise TypeError("ProgramImage collections must be tuples")
        if not isinstance(self.sections, tuple) or not isinstance(self.imports, tuple):
            raise TypeError("ProgramImage collections must be tuples")
        if not isinstance(self.exports, tuple) or not isinstance(self.relocations, tuple):
            raise TypeError("ProgramImage collections must be tuples")
        if not isinstance(self.findings, tuple):
            raise TypeError("ProgramImage findings must be a tuple")
        if self.source_size != len(self.source_bytes):
            raise ValueError("ProgramImage source size is inconsistent")
        if hashlib.sha256(self.source_bytes).hexdigest() != self.source_sha256:
            raise ValueError("ProgramImage source identity is inconsistent")
        if self.image_start < 0 or self.image_end < self.image_start or self.image_end > UINT32_END:
            raise ValueError("ProgramImage image extent is outside the 32-bit address space")
        if len(self.flat_bytes) != self.image_end - self.image_start:
            raise ValueError("ProgramImage flat image extent is inconsistent")

    @property
    def image_size(self):
        return self.image_end - self.image_start

    def _segment_for(self, address, size):
        if type(address) is not int or type(size) is not int or address < 0 or size < 0:
            return None
        end = address + size
        if end < address or end > UINT32_END:
            return None
        for segment in self.segments:
            if segment.guest_start <= address and end <= segment.guest_end:
                return segment
        return None

    def read_at_vaddr(self, address, size):
        """Read a wholly mapped segment span without exposing mutable storage."""
        segment = self._segment_for(address, size)
        if segment is None:
            return None
        if size == 0:
            return b""
        start = address - self.image_start
        return self.flat_bytes[start:start + size]

    def section(self, name):
        return next((section for section in self.sections if section.name == name), None)

    def as_dict(self):
        """Return deterministic JSON-shaped metadata without raw payload bytes."""
        return {
            "entry_point": self.entry_point,
            "elf_type": self.elf_type,
            "executable_intervals": [span.as_dict() for span in self.executable_intervals],
            "exports": [item.as_dict() for item in self.exports],
            "findings": [item.as_dict() for item in self.findings],
            "image_end": self.image_end,
            "image_start": self.image_start,
            "imports": [item.as_dict() for item in self.imports],
            "load_base": self.load_base,
            "machine": self.machine,
            "module": self.module.as_dict() if self.module else None,
            "relocations": [item.as_dict() for item in self.relocations],
            "schema_version": self.schema_version,
            "sections": [item.as_dict() for item in self.sections],
            "segments": [item.as_dict() for item in self.segments],
            "source": {
                "name": self.source_name,
                "sha256": self.source_sha256,
                "size": self.source_size,
            },
        }


def canonical_program_image_json(image):
    """Serialize image metadata deterministically without raw payload bytes."""
    if not isinstance(image, ProgramImage):
        raise TypeError("canonical_program_image_json requires a ProgramImage")
    import json
    return json.dumps(image.as_dict(), ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n"


# ``Span`` is a short compatibility spelling useful to callers constructing
# observation reports without importing an implementation-specific name.
Span = ProgramImageSpan


def _program_image_name(path):
    rendered = str(path).replace("\\", "/")
    return rendered.rsplit("/", 1)[-1] or "<memory>"


def _program_image_permissions(flags):
    return "".join(letter for bit, letter in ((4, "r"), (2, "w"), (1, "x")) if flags & bit)


def _program_image_address_kind(value, loads, *, endpoint=False):
    raw = False
    guest = False
    for segment in loads or ():
        raw_end = segment["vaddr"] + segment["memsz"]
        guest_end = segment["guest_end"]
        if endpoint:
            raw = raw or segment["vaddr"] <= value <= raw_end
            guest = guest or segment["guest_start"] <= value <= guest_end
        else:
            raw = raw or segment["vaddr"] <= value < raw_end
            guest = guest or segment["guest_start"] <= value < guest_end
    return raw, guest


def _program_image_ptr(value, base, loads=None):
    """Normalize a link-time or already-rebased guest pointer.

    PSP metadata is encountered in both forms: relocatable ELF fields contain
    link-time offsets, while some stripped/synthetic PRX inputs already carry
    absolute guest pointers. Prefer the validated raw/guest segment domains when
    available and retain the existing below-base fallback for external pointers.
    """
    if type(value) is not int or type(base) is not int or value < 0 or base < 0:
        return None
    if base:
        raw, guest = _program_image_address_kind(value, loads)
        if (raw and not guest) or (not raw and not guest and value < base):
            if value > UINT32_END - base:
                return None
            value += base
    if value >= UINT32_END:
        return None
    return value


def _program_image_end_ptr(value, base, loads=None):
    """Normalize an exclusive metadata endpoint, allowing UINT32_END."""
    if type(value) is not int or type(base) is not int or value < 0 or base < 0:
        return None
    if base:
        raw, guest = _program_image_address_kind(value, loads, endpoint=True)
        if (raw and not guest) or (not raw and not guest and value < base):
            if value > UINT32_END - base:
                return None
            value += base
    return value if value <= UINT32_END else None


def _program_image_rebased_range(start, end, base, label, findings, loads=None):
    if start == 0 and end == 0:
        return 0, 0
    if (start == 0) != (end == 0):
        findings.append(ProgramImageFinding(
            "module-range-invalid", "error", label,
            f"module range 0x{start:08x}..0x{end:08x} has only one null endpoint",
        ))
        return 0, 0
    rebased_start = _program_image_ptr(start, base, loads)
    rebased_end = _program_image_end_ptr(end, base, loads)
    if rebased_start is None or rebased_end is None or rebased_end < rebased_start:
        findings.append(ProgramImageFinding(
            "module-range-invalid", "error", label,
            f"module range 0x{start:08x}..0x{end:08x} is invalid",
        ))
        return 0, 0
    return rebased_start, rebased_end


def _program_image_raw_read(data, loads, address, size):
    """Read a guest span from validated raw segments, including BSS zeros."""
    if type(address) is not int or type(size) is not int or address < 0 or size < 0:
        return None
    end = address + size
    if end < address or end > UINT32_END:
        return None
    for segment in loads:
        start, finish = segment["guest_start"], segment["guest_end"]
        if start <= address and end <= finish:
            file_end = start + segment["filesz"]
            if end <= file_end:
                offset = segment["off"] + (address - start)
                return data[offset:offset + size]
            if address >= file_end:
                return bytes(size)
            first = data[segment["off"] + (address - start):segment["off"] + segment["filesz"]]
            return first + bytes(size - len(first))
    return None


def _program_image_cstr(data, loads, address, label, findings):
    if address == 0:
        return ""
    out = bytearray()
    for offset in range(MAX_PROGRAM_IMAGE_STRING_BYTES + 1):
        raw = _program_image_raw_read(data, loads, address + offset, 1)
        if raw is None or len(raw) != 1:
            findings.append(ProgramImageFinding(
                "string-oob", "error", label,
                f"guest string at 0x{address:08x} leaves a validated segment",
            ))
            return ""
        if raw[0] == 0:
            try:
                return bytes(out).decode("ascii")
            except UnicodeDecodeError:
                return bytes(out).decode("ascii", "backslashreplace")
        out.append(raw[0])
    findings.append(ProgramImageFinding(
        "string-overlong", "error", label,
        f"guest string at 0x{address:08x} exceeds {MAX_PROGRAM_IMAGE_STRING_BYTES} bytes without NUL",
    ))
    return ""


def _program_image_table(data, loads, start, end, label, findings, base=0, mode="export"):
    """Read a bounded PSP library-entry table without mutating the image."""
    if start == 0 and end == 0:
        return []
    if start < 0 or end < start or end > UINT32_END or end - start > MAX_PROGRAM_IMAGE_TABLE_BYTES:
        findings.append(ProgramImageFinding(
            "table-span-invalid", "error", label,
            f"table span 0x{start:08x}..0x{end:08x} is reversed or exceeds the table bound",
        ))
        return []
    entries = []
    position = start
    while position < end:
        raw = _program_image_raw_read(data, loads, position, 20)
        if raw is None or len(raw) != 20:
            findings.append(ProgramImageFinding(
                "table-entry-oob", "error", label,
                f"table entry at 0x{position:08x} is not wholly mapped",
            ))
            break
        name_ptr, version, flags, size_words, num_vars, num_funcs, table_a, table_b = struct.unpack(
            "<IHHBBHII", raw
        )
        entry_size = size_words * 4
        if size_words == 0 or entry_size < 20 or position + entry_size > end:
            findings.append(ProgramImageFinding(
                "table-entry-size-invalid", "error", label,
                f"table entry at 0x{position:08x} has invalid size {size_words} words",
            ))
            break
        if name_ptr:
            name_address = _program_image_ptr(name_ptr, base, loads)
            if name_address is None:
                findings.append(ProgramImageFinding(
                    "pointer-value-oob", "error", f"{label}.name",
                    f"library-name pointer 0x{name_ptr:08x} cannot be represented",
                ))
                name = "(invalid)"
            else:
                name = _program_image_cstr(data, loads, name_address, f"{label}.name", findings)
        else:
            name = "(null)"

        def rebase_table_pointer(pointer, pointer_label):
            if not pointer:
                return 0
            rebased = _program_image_ptr(pointer, base, loads)
            if rebased is None:
                findings.append(ProgramImageFinding(
                    "pointer-value-oob", "error", pointer_label,
                    f"pointer value 0x{pointer:08x} cannot be represented",
                ))
                return 0
            return rebased

        table_a = rebase_table_pointer(table_a, f"{label}.table_a")
        table_b = rebase_table_pointer(table_b, f"{label}.table_b")
        if num_funcs:
            count_bytes = num_funcs * 4
            if not table_a or _program_image_raw_read(data, loads, table_a, count_bytes) is None:
                findings.append(ProgramImageFinding(
                    "table-function-oob", "error", label,
                    f"function/NID table at 0x{table_a:08x} has {num_funcs} entries outside a segment",
                ))
            if mode == "import" and (not table_b or _program_image_raw_read(data, loads, table_b, count_bytes) is None):
                findings.append(ProgramImageFinding(
                    "table-stub-oob", "error", label,
                    f"stub table at 0x{table_b:08x} has {num_funcs} entries outside a segment",
                ))
        if mode == "export" and num_vars:
            count_bytes = num_vars * 4
            if not table_b or _program_image_raw_read(data, loads, table_b, count_bytes) is None:
                findings.append(ProgramImageFinding(
                    "table-variable-oob", "error", label,
                    f"variable table at 0x{table_b:08x} has {num_vars} entries outside a segment",
                ))
        entries.append((position, name, version, flags, num_vars, num_funcs, table_a, table_b))
        position += entry_size
    return entries


def _program_image_u32_table(data, loads, address, count, label, findings):
    if count == 0:
        return ()
    if not address or count > MAX_PROGRAM_IMAGE_TABLE_BYTES // 4:
        findings.append(ProgramImageFinding(
            "pointer-table-invalid", "error", label,
            f"pointer table at 0x{address:08x} has invalid count {count}",
        ))
        return ()
    raw = _program_image_raw_read(data, loads, address, count * 4)
    if raw is None or len(raw) != count * 4:
        findings.append(ProgramImageFinding(
            "pointer-table-oob", "error", label,
            f"pointer table at 0x{address:08x} is not wholly mapped",
        ))
        return ()
    return struct.unpack(f"<{count}I", raw)


def _program_image_validate_packed_relocation(data, envelope, loads, section, label, findings):
    """Run the legacy packed-relocation decoder against a non-mutating probe.

    Packed PSP relocation streams are retained opaquely in ProgramImage v1, but
    their command/table bounds still need validation before future consumers can
    trust the image. The probe reuses the established decoder while making reads
    checked against the validated segments and making writes no-ops.
    """
    segment_index = next(
        (
            program["idx"]
            for program in envelope["phdrs"]
            if program["type"] == 1
            and program["off"] <= section["off"] < program["off"] + program["filesz"]
        ),
        None,
    )
    if segment_index is None:
        findings.append(ProgramImageFinding(
            "relocation-packed-invalid", "error", label,
            "packed relocation section is not associated with a load segment",
        ))
        return

    class _PackedProbe:
        def __init__(self):
            self.data = data
            self.segments = envelope["phdrs"]
            self.seg_vaddr = [item["guest_start"] for item in loads]

        def r32(self, address):
            raw = _program_image_raw_read(data, loads, address, 4)
            if raw is None or len(raw) != 4:
                raise ValueError(f"packed relocation target 0x{address:08x} is unmapped")
            return struct.unpack("<I", raw)[0]

        def w32(self, _address, _value):
            return None

    try:
        Prx._apply_packed(_PackedProbe(), {
            "seg_idx": segment_index,
            "off": section["off"],
            "size": section["size"],
        })
    except (IndexError, struct.error, ValueError) as exc:
        findings.append(ProgramImageFinding(
            "relocation-packed-invalid", "error", label, str(exc)
        ))


def _program_image_validate_relocations(data, envelope, loads, base, findings):
    """Validate relocation records and retain them without applying them."""
    records = []
    load_starts = [segment["guest_start"] for segment in loads]
    for section in envelope["shdrs"]:
        section_type = section["typ"]
        if section_type not in (4, 9, SHT_PRX_RELOC, SHT_PRX_RELOC_PACKED):
            continue
        size = section["size"]
        label = f"section[{section['idx']}]"
        if size > MAX_PROGRAM_IMAGE_TABLE_BYTES:
            findings.append(ProgramImageFinding(
                "relocation-table-too-large", "error", label,
                f"relocation table is larger than {MAX_PROGRAM_IMAGE_TABLE_BYTES} bytes",
            ))
            continue
        if section_type == SHT_PRX_RELOC_PACKED:
            if size < 4:
                findings.append(ProgramImageFinding(
                    "relocation-table-truncated", "error", label,
                    "packed relocation table is shorter than its header",
                ))
            else:
                _program_image_validate_packed_relocation(
                    data, envelope, loads, section, label, findings
                )
            # The packed PSP stream remains opaque in the canonical record until
            # the legacy relocation representation is migrated. Its bounded file
            # span and decoder validation are still represented explicitly.
            records.append(ProgramImageRelocation(
                section["idx"], section["off"], section_type, 0, 0, None
            ))
            continue
        expected = 12 if section_type == 4 else 8
        entry_size = section["entsz"] or expected
        if entry_size < expected or size % entry_size:
            findings.append(ProgramImageFinding(
                "relocation-table-shape", "error", label,
                f"relocation section size 0x{size:x} is not a multiple of entry size {entry_size}",
            ))
            continue
        for index in range(size // entry_size):
            source_offset = section["off"] + index * entry_size
            if section_type == 4:
                offset, info, _addend = struct.unpack_from("<IIi", data, source_offset)
            else:
                offset, info = struct.unpack_from("<II", data, source_offset)
            target = None
            target_segment_valid = True
            if section_type == SHT_PRX_RELOC:
                offset_segment = (info >> 8) & 0xFF
                target_segment = (info >> 16) & 0xFF
                if offset_segment >= len(load_starts):
                    target_segment_valid = False
                    findings.append(ProgramImageFinding(
                        "relocation-segment-oob", "error", f"{label}.relocation[{index}]",
                        f"relocation offset segment {offset_segment} is outside the load-segment table",
                    ))
                if target_segment >= len(load_starts):
                    target_segment_valid = False
                    findings.append(ProgramImageFinding(
                        "relocation-segment-oob", "error", f"{label}.relocation[{index}]",
                        f"relocation target segment {target_segment} is outside the load-segment table",
                    ))
                if target_segment_valid and offset > UINT32_END - load_starts[offset_segment]:
                    findings.append(ProgramImageFinding(
                        "relocation-target-oob", "error", f"{label}.relocation[{index}]",
                        f"relocation offset 0x{offset:08x} wraps the guest address space",
                    ))
                elif target_segment_valid:
                    target = load_starts[offset_segment] + offset
            else:
                target = _program_image_ptr(offset, base, loads)
            if target_segment_valid and (
                target is None or _program_image_raw_read(data, loads, target, 4) is None
            ):
                findings.append(ProgramImageFinding(
                    "relocation-target-oob", "error", f"{label}.relocation[{index}]",
                    f"relocation target 0x{offset:08x} is outside validated load segments",
                ))
                target = None
            records.append(ProgramImageRelocation(
                section["idx"], source_offset, info & 0xFF, offset, info, target
            ))
    return records


def _program_image_section_name(data, envelope, section):
    shstr = envelope["shdrs"][envelope["shstrndx"]]
    start = shstr["off"] + section["name"]
    finish = data.find(b"\0", start, shstr["off"] + shstr["size"])
    return data[start:finish].decode("ascii", "replace") if finish >= 0 else ""


def _program_image_rebased_values(values, base, label, findings, loads=None, data=None):
    result = []
    for index, value in enumerate(values):
        if value == 0:
            result.append(0)
            continue
        address = _program_image_ptr(value, base, loads)
        if address is None:
            findings.append(ProgramImageFinding(
                "pointer-value-oob", "error", f"{label}[{index}]",
                f"pointer value 0x{value:08x} cannot be represented in the guest address space",
            ))
            result.append(0)
        elif data is not None and _program_image_raw_read(data, loads, address, 1) is None:
            findings.append(ProgramImageFinding(
                "pointer-target-oob", "error", f"{label}[{index}]",
                f"pointer value 0x{value:08x} resolves to unmapped guest address 0x{address:08x}",
            ))
            result.append(0)
        else:
            result.append(address)
    return tuple(result)


def load_program_image(path, base=0, psp_header=None):
    """Validate one ELF/PRX and return an immutable :class:`ProgramImage`.

    This adapter performs no relocation or HST-specific code-generation work.
    Structural validation, including metadata table bounds, completes before
    the flat image is allocated or filled. Invalid input raises a structured
    :class:`ProgramImageValidationError` containing every finding collected.
    """
    try:
        with open(path, "rb") as source:
            data = source.read(MAX_ELF_FILE_BYTES + 1)
    except (OSError, TypeError, ValueError) as exc:
        raise ProgramImageValidationError((ProgramImageFinding(
            "source-read", "error", str(path), str(exc)
        ),)) from exc
    if len(data) > MAX_ELF_FILE_BYTES:
        raise ProgramImageValidationError((ProgramImageFinding(
            "source-too-large", "error", str(path),
            f"ELF input exceeds the {MAX_ELF_FILE_BYTES}-byte bound",
        ),))

    findings = []
    try:
        envelope = validate_elf32_envelope(data, str(path))
    except (OSError, ValueError, struct.error) as exc:
        raise ProgramImageValidationError((ProgramImageFinding(
            "elf-envelope", "error", str(path), str(exc)
        ),)) from exc

    valid_base = type(base) is int and 0 <= base < UINT32_END
    effective_base = base if valid_base else 0
    if not valid_base:
        findings.append(ProgramImageFinding(
            "load-base-invalid", "error", "load_base",
            "load base is outside the 32-bit address space",
        ))

    load_specs = []
    for program in envelope["phdrs"]:
        if program["type"] != 1:
            continue
        spec = dict(program)
        align = spec["align"]
        if align not in (0, 1) and (align & (align - 1)) != 0:
            findings.append(ProgramImageFinding(
                "alignment-invalid", "error", f"PT_LOAD[{spec['idx']}].p_align",
                f"alignment 0x{align:x} is not a power of two",
            ))
        if align > 1 and (spec["off"] - spec["vaddr"]) % align:
            findings.append(ProgramImageFinding(
                "alignment-invalid", "error", f"PT_LOAD[{spec['idx']}].p_align",
                "file offset and virtual address are not congruent at p_align",
            ))
        try:
            checked_span(len(data), spec["off"], spec["filesz"], f"PT_LOAD[{spec['idx']}] source")
        except ValueError as exc:
            findings.append(ProgramImageFinding(
                "segment-source-oob", "error", f"PT_LOAD[{spec['idx']}].p_offset", str(exc)
            ))

        load_specs.append(spec)

    if not load_specs:
        findings.append(ProgramImageFinding(
            "no-load-segments", "error", "program_headers", "ELF has no PT_LOAD segments"
        ))

    psp_sizes = None
    psp_bss_size = 0
    if psp_header is not None and load_specs:
        try:
            psp_sizes, psp_bss_size = read_psp_segment_sizes(psp_header, len(load_specs))
        except (OSError, ValueError, struct.error) as exc:
            findings.append(ProgramImageFinding(
                "psp-header-invalid", "error", "psp_header", str(exc)
            ))

    if psp_sizes is not None:
        declared_extra = 0
        for spec, declared_size in zip(load_specs, psp_sizes):
            if declared_size < spec["filesz"]:
                findings.append(ProgramImageFinding(
                    "psp-segment-size-invalid", "error", f"PT_LOAD[{spec['idx']}].memsz",
                    "PSP header declares less memory than the ELF file payload",
                ))
            spec["memsz"] = max(spec["memsz"], declared_size)
            declared_extra += max(0, declared_size - spec["filesz"])
        if declared_extra < psp_bss_size:
            findings.append(ProgramImageFinding(
                "psp-bss-invalid", "error", "psp_header.bss_size",
                "PSP segment declarations do not cover the declared BSS extent",
            ))

    loads = []
    for spec in load_specs:
        try:
            start, end = image_extent(
                effective_base, spec["vaddr"], spec["memsz"], f"PT_LOAD {spec['idx']}"
            )
        except ValueError as exc:
            findings.append(ProgramImageFinding(
                "guest-destination-oob", "error", f"PT_LOAD[{spec['idx']}].p_vaddr", str(exc)
            ))
            continue
        loads.append({**spec, "guest_start": start, "guest_end": end})

    by_guest = sorted(loads, key=lambda item: (item["guest_start"], item["guest_end"], item["idx"]))
    for left, right in zip(by_guest, by_guest[1:]):
        if right["guest_start"] < left["guest_end"]:
            findings.append(ProgramImageFinding(
                "segment-overlap", "error", "program_headers",
                f"PT_LOAD[{left['idx']}] overlaps PT_LOAD[{right['idx']}] in guest memory",
            ))
    by_file = sorted(
        (item for item in loads if item["filesz"]),
        key=lambda item: (item["off"], item["off"] + item["filesz"], item["idx"]),
    )
    for left, right in zip(by_file, by_file[1:]):
        if right["off"] < left["off"] + left["filesz"]:
            findings.append(ProgramImageFinding(
                "segment-file-overlap", "error", "program_headers",
                f"PT_LOAD[{left['idx']}] overlaps PT_LOAD[{right['idx']}] in source bytes",
            ))

    executable_intervals = []
    for segment in loads:
        if segment["flags"] & 1 and segment["guest_end"] > segment["guest_start"]:
            executable_intervals.append((segment["guest_start"], segment["guest_end"]))
    executable_intervals.sort()
    merged_exec = []
    for start, end in executable_intervals:
        if merged_exec and start <= merged_exec[-1][1]:
            merged_exec[-1] = (merged_exec[-1][0], max(merged_exec[-1][1], end))
        else:
            merged_exec.append((start, end))

    image_start = min((item["guest_start"] for item in loads), default=0)
    image_end = max((item["guest_end"] for item in loads), default=0)
    if image_end < image_start or image_end - image_start > MAX_ELF_IMAGE_BYTES:
        findings.append(ProgramImageFinding(
            "image-extent-invalid", "error", "program_headers",
            "the flat image extent exceeds the supported bound",
        ))

    sections = []
    section_by_name = {}
    if envelope["shnum"]:
        for raw_section in envelope["shdrs"]:
            name = _program_image_section_name(data, envelope, raw_section)
            address = (
                0 if raw_section["idx"] == 0
                else _program_image_ptr(raw_section["addr"], effective_base, loads)
            )
            if address is None:
                findings.append(ProgramImageFinding(
                    "section-address-oob", "error", f"section[{raw_section['idx']}].sh_addr",
                    f"section address 0x{raw_section['addr']:08x} cannot be represented",
                ))
                address = 0
            if raw_section["size"] and address > UINT32_END - raw_section["size"]:
                findings.append(ProgramImageFinding(
                    "section-address-span-oob", "error", f"section[{raw_section['idx']}].sh_addr",
                    f"section address span 0x{address:08x}..0x{address + raw_section['size']:x} "
                    "exceeds the 32-bit address space",
                ))
            section = ProgramImageSection(
                raw_section["idx"], name, raw_section["typ"], raw_section["flags"],
                address, raw_section["size"], raw_section["off"], raw_section["entsz"],
            )
            sections.append(section)
            section_by_name[name] = section

    entry_point = _program_image_ptr(envelope["entry"], effective_base, loads)
    if entry_point is None:
        findings.append(ProgramImageFinding(
            "entry-point-oob", "error", "e_entry",
            f"entry point 0x{envelope['entry']:08x} cannot be represented",
        ))
        entry_point = 0
    elif entry_point & 3:
        findings.append(ProgramImageFinding(
            "entry-point-unaligned", "error", "e_entry",
            f"entry point 0x{entry_point:08x} is not instruction aligned",
        ))
    elif not any(segment["guest_start"] <= entry_point < segment["guest_end"] for segment in loads):
        findings.append(ProgramImageFinding(
            "entry-point-unmapped", "error", "e_entry",
            f"entry point 0x{entry_point:08x} is outside every load segment",
        ))
    elif not any(
        segment["guest_start"] <= entry_point < segment["guest_end"] and segment["flags"] & 1
        for segment in loads
    ):
        findings.append(ProgramImageFinding(
            "entry-point-nonexec", "error", "e_entry",
            f"entry point 0x{entry_point:08x} is not in an executable segment",
        ))

    module = None
    imports = []
    exports = []
    module_section = section_by_name.get(".rodata.sceModuleInfo")
    if module_section is not None:
        raw_module = _program_image_raw_read(data, loads, module_section.address, 52)
        if raw_module is None or len(raw_module) != 52:
            findings.append(ProgramImageFinding(
                "module-info-oob", "error", ".rodata.sceModuleInfo",
                "module metadata is not wholly mapped by a validated load segment",
            ))
        else:
            attr, version, name_bytes, gp, ent_start_raw, ent_end_raw, stub_start_raw, stub_end_raw = struct.unpack(
                "<HH28s5I", raw_module
            )
            nul = name_bytes.find(b"\0")
            if nul < 0:
                findings.append(ProgramImageFinding(
                    "module-name-overlong", "error", ".rodata.sceModuleInfo.modname",
                    "module name has no NUL terminator in its fixed-width field",
                ))
                module_name = name_bytes.decode("ascii", "backslashreplace")
            else:
                module_name = name_bytes[:nul].decode("ascii", "backslashreplace")

            ent_start, ent_end = _program_image_rebased_range(
                ent_start_raw, ent_end_raw, effective_base, "module.exports", findings, loads
            )
            stub_start, stub_end = _program_image_rebased_range(
                stub_start_raw, stub_end_raw, effective_base, "module.imports", findings, loads
            )
            module_gp = _program_image_ptr(gp, effective_base, loads) if gp else 0
            if module_gp is None or (
                module_gp and _program_image_raw_read(data, loads, module_gp, 1) is None
            ):
                findings.append(ProgramImageFinding(
                    "module-gp-oob", "error", ".rodata.sceModuleInfo.gp",
                    f"module GP pointer 0x{gp:08x} is outside validated load segments",
                ))
                module_gp = 0
            module = ProgramImageModule(
                module_name, attr, version, module_gp,
                ent_start, ent_end, stub_start, stub_end,
            )
            export_entries = _program_image_table(
                data, loads, ent_start, ent_end, "module.exports", findings, effective_base, "export"
            )
            import_entries = _program_image_table(
                data, loads, stub_start, stub_end, "module.imports", findings, effective_base, "import"
            )
            for position, libname, version, flags, num_vars, num_funcs, table_a, table_b in export_entries:
                function_values = _program_image_u32_table(
                    data, loads, table_a, num_funcs, f"module.exports[0x{position:08x}].functions", findings
                )
                variable_values = _program_image_u32_table(
                    data, loads, table_b, num_vars, f"module.exports[0x{position:08x}].variables", findings
                )
                exports.append(ProgramImageExport(
                    libname, position, num_funcs, num_vars, table_a, table_b,
                    _program_image_rebased_values(
                        function_values, effective_base, "export.functions", findings, loads, data
                    ),
                    _program_image_rebased_values(
                        variable_values, effective_base, "export.variables", findings, loads, data
                    ),
                ))
            for position, libname, version, flags, num_vars, num_funcs, table_a, table_b in import_entries:
                nids = _program_image_u32_table(
                    data, loads, table_a, num_funcs, f"module.imports[0x{position:08x}].nids", findings
                )
                imports.append(ProgramImageImport(
                    libname, position, num_funcs, table_a, table_b, tuple(nids)
                ))

    relocations = _program_image_validate_relocations(data, envelope, loads, effective_base, findings)
    errors = tuple(item for item in findings if item.severity == "error")
    if errors:
        raise ProgramImageValidationError(tuple(findings))

    segment_models = []
    image = bytearray(image_end - image_start)
    for segment in loads:
        source_end = segment["off"] + segment["filesz"]
        destination = segment["guest_start"] - image_start
        payload = data[segment["off"]:source_end]
        image[destination:destination + len(payload)] = payload
        zero_start = segment["guest_start"] + segment["filesz"]
        zero_fill = (
            ProgramImageSpan(zero_start, segment["guest_end"])
            if zero_start < segment["guest_end"] else None
        )
        segment_models.append(ProgramImageSegment(
            segment["idx"], segment["off"], segment["filesz"], segment["memsz"],
            segment["vaddr"], segment["guest_start"], segment["guest_end"],
            _program_image_permissions(segment["flags"]), segment["align"],
            ProgramImageSpan(segment["off"], source_end),
            ProgramImageSpan(segment["guest_start"], segment["guest_end"]),
            zero_fill,
        ))

    return ProgramImage(
        PROGRAM_IMAGE_SCHEMA_VERSION, _program_image_name(path),
        hashlib.sha256(data).hexdigest(), len(data), envelope["e_type"],
        envelope.get("machine", struct.unpack_from("<H", data, 18)[0]), effective_base,
        entry_point, tuple(segment_models),
        tuple(ProgramImageSpan(start, end) for start, end in merged_exec),
        tuple(sections), tuple(imports), tuple(exports), tuple(relocations), module,
        tuple(findings), image_start, image_end, bytes(data), bytes(image),
    )


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
            self.data = f.read(MAX_ELF_FILE_BYTES + 1)
        if len(self.data) > MAX_ELF_FILE_BYTES:
            raise ValueError(f"{path}: ELF file exceeds the 256 MiB input bound")
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
            if rtype in _TYPE_A_KNOWN_UNSUPPORTED_TYPES or rtype not in _TYPE_A_RECOGNIZED_TYPES:
                raise ValueError(
                    f"unsupported Type-A relocation type 0x{rtype:x} "
                    f"at offset 0x{offset:08x} "
                    f"(offset segment {ofs_seg}, target segment {addr_seg})"
                )
            addr = offset + self.seg_vaddr[ofs_seg]
            relocate_to = self.seg_vaddr[addr_seg]
            op = self.r32(addr)

            if rtype in _TYPE_A_NOOP_TYPES:
                # NONE and GPREL16 are deliberate no-ops in this loader.
                continue
            elif rtype in _TYPE_A_SPECIAL_TYPES:
                # The PSP's R_MIPS_LITERAL (8) is a recognized,
                # diagnostic-only relocation category, not an unknown type to
                # reject or a value to rewrite in this loader.
                continue
            elif rtype == R_MIPS_32:
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
