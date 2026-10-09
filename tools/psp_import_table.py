# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Defensive PSP ELF import- and export-table parser.

Used by the import-coverage audit gate and by the guest-module planner.
Standalone on purpose: tools/analyze.py's Elf class is a trusting pipeline
loader for a known-good local ELF, while this module is fed arbitrary
developer-supplied byte buffers (and deliberately malformed CI fixtures).
Every read is bounds-checked against the file, every guest address is mapped
through validated PT_LOAD/section ranges, and every failure raises
ImportTableError with a message instead of crashing, wrapping, or allocating
based on unvalidated lengths. Nothing here executes or disassembles guest
code; the output is (library name, function NID) pairs plus stub addresses
for imports, and (library name, attributes, exported NIDs) for exports.

The reading and the structural checks are this module's own. The layout
model that pairs stub slots with NIDs (layout_import_windows) is shared with
tools/imports.py, the analyzer's code-generation import map, so both parsers
accept and refuse the same retail layouts and map their slots identically.
The parity tests are AuditAnalyzerParityTests in tools/test_imports.py.

Layout references: psp-fixup-imports (pspsdk/tools) builds the table the PSP
kernel loader consumes: .sceStub.text holds one 8-byte slot per imported
function, .rodata.sceNid holds one 4-byte NID per function, and the two
arrays pair globally by position (psp-fixup-imports aborts when a slot's
embedded NID differs from the section NID). SceModuleInfo.libstub..libstubend
holds one PspLibStubEntry per library naming a run of numFuncs consecutive
positions. When the linker interleaves stub libraries (the "stubs out of
order" case psp-fixup-imports warns about) the runs overlap and trailing
positions can be left unclaimed; the loader patches only covered positions.
This parser therefore pairs slots with NIDs globally, attributes library
names from the window runs (last claimer wins on overlap), marks unclaimed
slots with the UNATTRIBUTED_LIBRARY marker, and reports structural findings;
it fails closed on malformed bounds, truncated records, overflow, impossible
counts, and windows whose NID position disagrees with their stub position.

Exports use the PSP SceLibraryEntryTable layout the module loader consumes
(the same layout src/rt/hle.c register_prx_exports() publishes when it loads
a guest module): SceModuleInfo.ent_top..ent_end holds one entry per exported
library, each starting with a 16-byte header (library-name pointer, version,
attribute, entry length in words, variable count, function count, entry
table pointer). The entry table holds every function NID, then every
variable NID, then the matching guest addresses in the same order. The
nameless entry carrying the module-lifecycle exports (module_start,
module_info, ...) has the syslib attribute and is never importable.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
import struct
from typing import NamedTuple

# Hard resource caps. A malformed header must not be able to request work or
# memory proportional to a forged length field.
MAX_FILE_SIZE = 256 * 1024 * 1024
MAX_SEGMENTS = 512
# e_shnum is a 16-bit field and the section-header table must lie inside the
# file (checked below), which bounds the work for any count; the analyzer's ELF
# envelope (tools/elf_bounds.py) imposes no lower count cap, so neither does this.
MAX_SECTIONS = 0xFFFF
MAX_LIBRARIES = 256
MAX_FUNCS_PER_LIB = 4096
MAX_TOTAL_FUNCS = 65536
MAX_LIBNAME_LEN = 128
# PspLibStubEntry.size is in 32-bit words; 5 covers the fields we decode and
# real tables use 5 or 6. Anything outside a small window is hostile/corrupt.
MIN_STUB_ENTRY_WORDS = 5
MAX_STUB_ENTRY_WORDS = 32
# SceLibraryEntryTable.len is in 32-bit words; the 16-byte header is the
# minimum and real tables use 4 to 6. The upper bound and the export-table span
# cap are the runtime loader's own (src/rt/hle.c register_prx_exports), so the
# planner never refuses a table the runtime would publish.
MIN_EXPORT_ENTRY_WORDS = 4
MAX_EXPORT_ENTRY_WORDS = 0x40
MAX_EXPORT_TABLE_BYTES = 0x10000
EXPORT_ENTRY_HEADER_SIZE = 16

# SceModuleInfo.modattribute bit for a kernel-mode module.
MODULE_ATTR_KERNEL = 0x1000
# SceLibraryEntryTable.attribute bits. A kernel-mode module's library is
# callable from user mode only through the syscall-export bit; the syslib bit
# marks the nameless module-lifecycle entry, which no module can import.
LIB_ATTR_SYSCALL_EXPORT = 0x4000
LIB_ATTR_SYSLIB = 0x8000

MODULE_INFO_SECTION = b".rodata.sceModuleInfo"
MODULE_INFO_SIZE = 52
MODULE_NAME_BYTES = 28
STUB_ENTRY_HEADER_SIZE = 20
STUB_SECTION = b".sceStub.text"
NID_SECTION = b".rodata.sceNid"
U32_MAX = 0xFFFFFFFF

# Marker for stub slots that no library window claims (interleaved stub
# tables). Library-name reads are restricted to printable ASCII, so this
# literal can never collide with a real guest name.
UNATTRIBUTED_LIBRARY = "(unattributed)"

# Boundary codes the analyzer (tools/imports.py) also names. Only refusals that
# the shared layout model or the shared variable check raise carry one.
CODE_TABLE_INVALID = "ANALYZER_IMPORT_TABLE_INVALID"
CODE_NID_TABLE_MISSING = "ANALYZER_IMPORT_NID_TABLE_MISSING"
CODE_LIBRARY_NAME_UNMAPPED = "ANALYZER_IMPORT_LIBRARY_NAME_UNMAPPED"
CODE_REGIONS_MISMATCH = "ANALYZER_IMPORT_REGIONS_MISMATCH"
CODE_VARIABLE_IMPORTS_UNSUPPORTED = "ANALYZER_VARIABLE_IMPORTS_UNSUPPORTED"


class ImportTableError(Exception):
    """Malformed or out-of-policy input. Always carries a human-usable message.

    code is the analyzer's boundary code when the refusal is one the analyzer
    also names, else None.
    """

    def __init__(self, message: str, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class ImportedFunc:
    library: str
    nid: int
    stub_addr: int


@dataclass
class ImportTable:
    funcs: list[ImportedFunc] = field(default_factory=list)
    libraries: list[str] = field(default_factory=list)
    findings: list[str] = field(default_factory=list)
    # SceModuleInfo.attribute (the first u16 of the module info record). The
    # PSP_MODULE_KERNEL bit (0x1000) marks a kernel-mode module; every other
    # module runs in user mode and may import only user (syscall) libraries.
    module_attributes: int = 0


@dataclass(frozen=True)
class ExportedLibrary:
    """One SceLibraryEntryTable entry: a library and the NIDs it exports."""

    name: str | None  # None for the nameless module-lifecycle (syslib) entry
    attributes: int
    function_nids: tuple[int, ...]
    variable_nids: tuple[int, ...]


@dataclass(frozen=True)
class ExportTable:
    module_attributes: int
    libraries: tuple[ExportedLibrary, ...]

    @property
    def kernel_mode(self) -> bool:
        return bool(self.module_attributes & MODULE_ATTR_KERNEL)

    def user_callable_libraries(self) -> tuple[ExportedLibrary, ...]:
        """Libraries a user-mode importer can link against.

        Never the syslib entry (it carries the module's own lifecycle
        exports and has no library name to import by). A user-mode module's
        named libraries are all directly importable; a kernel-mode module's
        library reaches user mode only when it carries the syscall-export
        attribute, and the rest are importable by kernel code alone.
        """
        return tuple(
            library
            for library in self.libraries
            if library.name is not None
            and not library.attributes & LIB_ATTR_SYSLIB
            and (not self.kernel_mode or library.attributes & LIB_ATTR_SYSCALL_EXPORT)
        )


def _need(data: bytes, off: int, n: int, what: str) -> bytes:
    """Return data[off:off+n], refusing negative/overflowing/short reads."""
    if off < 0 or n < 0 or off + n > len(data):
        raise ImportTableError(
            f"{what}: needs bytes [{off:#x}, {off + n:#x}) but file is {len(data):#x} bytes"
        )
    return data[off : off + n]


class _GuestMap:
    """vaddr -> file offset translation built from validated load ranges."""

    def __init__(self) -> None:
        self.ranges: list[tuple[int, int, int]] = []  # (vaddr, size, file_off)

    def add(self, vaddr: int, size: int, file_off: int, file_size: int, what: str) -> None:
        if size == 0:
            return
        if vaddr > U32_MAX or size > U32_MAX or vaddr + size > U32_MAX + 1:
            raise ImportTableError(f"{what}: guest range {vaddr:#x}+{size:#x} wraps the 32-bit space")
        if file_off + size > file_size:
            raise ImportTableError(
                f"{what}: file range {file_off:#x}+{size:#x} exceeds file size {file_size:#x}"
            )
        self.ranges.append((vaddr, size, file_off))

    def to_off(self, vaddr: int, n: int, what: str) -> int:
        if vaddr > U32_MAX or n < 0 or vaddr + n > U32_MAX + 1:
            raise ImportTableError(f"{what}: guest address {vaddr:#x}+{n:#x} wraps the 32-bit space")
        for base, size, file_off in self.ranges:
            if base <= vaddr and vaddr + n <= base + size:
                return file_off + (vaddr - base)
        raise ImportTableError(f"{what}: guest address {vaddr:#x}+{n:#x} is not in any loaded range")


def _read_guest(data: bytes, gmap: _GuestMap, vaddr: int, n: int, what: str) -> bytes:
    return _need(data, gmap.to_off(vaddr, n, what), n, what)


def _read_guest_cstr(data: bytes, gmap: _GuestMap, vaddr: int, what: str) -> str:
    out = bytearray()
    for i in range(MAX_LIBNAME_LEN):
        ch = _read_guest(data, gmap, vaddr + i, 1, what)[0]
        if ch == 0:
            if not out:
                raise ImportTableError(f"{what}: empty library name at {vaddr:#x}")
            return out.decode("latin1")
        if ch < 0x20 or ch > 0x7E:
            raise ImportTableError(f"{what}: non-printable byte {ch:#04x} in name at {vaddr:#x}")
        out.append(ch)
    raise ImportTableError(f"{what}: name at {vaddr:#x} is unterminated within {MAX_LIBNAME_LEN} bytes")


def _parse_elf_maps(data: bytes) -> tuple[_GuestMap, int | None, int | None, dict[str, tuple[int, int]]]:
    """Validate the ELF envelope.

    Returns (guest map, module-info vaddr or None, phdr[0].p_paddr or None,
    section map {name: (sh_addr, sh_size)}).
    The module-info vaddr comes from the .rodata.sceModuleInfo section when
    section headers name one; sectionless (stripped) PRX/ELF inputs instead
    locate SceModuleInfo through the PRX convention -- phdr[0].p_paddr with
    the kernel bit masked is the module info's *file offset* -- which the
    caller validates before use.
    """
    if len(data) > MAX_FILE_SIZE:
        raise ImportTableError(f"file is {len(data)} bytes; refusing inputs over {MAX_FILE_SIZE}")
    eh = _need(data, 0, 52, "ELF header")
    if eh[0:4] != b"\x7fELF":
        raise ImportTableError("not an ELF file (bad magic)")
    if eh[4] != 1 or eh[5] != 1:
        raise ImportTableError("not a 32-bit little-endian ELF (PSP requires ELFCLASS32/ELFDATA2LSB)")
    (e_machine,) = struct.unpack_from("<H", eh, 18)
    if e_machine != 8:
        raise ImportTableError(f"e_machine {e_machine} is not MIPS (8)")
    e_phoff, e_shoff = struct.unpack_from("<II", eh, 28)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", eh, 42)

    gmap = _GuestMap()
    phdr0_paddr: int | None = None
    if e_phnum:
        if e_phnum > MAX_SEGMENTS:
            raise ImportTableError(f"e_phnum {e_phnum} exceeds cap {MAX_SEGMENTS}")
        if e_phentsize != 32:
            raise ImportTableError(f"e_phentsize {e_phentsize} is not 32")
        for i in range(e_phnum):
            ph = _need(data, e_phoff + i * 32, 32, f"program header {i}")
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<IIIIII", ph, 0)
            if i == 0:
                phdr0_paddr = p_paddr
            # Same envelope as tools/elf_bounds.py: every segment's file span is
            # checked, even an empty one, and a PT_LOAD cannot carry more file
            # bytes than memory.
            if p_offset + p_filesz > len(data):
                raise ImportTableError(
                    f"program header {i}: file range {p_offset:#x}+{p_filesz:#x} "
                    f"exceeds file size {len(data):#x}"
                )
            if p_type == 1 and p_filesz > p_memsz:  # PT_LOAD
                raise ImportTableError(f"program header {i}: PT_LOAD filesz exceeds memsz")
            if p_type == 1:  # PT_LOAD
                gmap.add(p_vaddr, p_filesz, p_offset, len(data), f"program header {i}")

    modinfo_vaddr: int | None = None
    sections: dict[str, tuple[int, int]] = {}
    if e_shnum:
        if e_shnum > MAX_SECTIONS:
            raise ImportTableError(f"e_shnum {e_shnum} exceeds cap {MAX_SECTIONS}")
        if e_shentsize != 40:
            raise ImportTableError(f"e_shentsize {e_shentsize} is not 40")
        if e_shstrndx >= e_shnum:
            raise ImportTableError(f"e_shstrndx {e_shstrndx} out of range ({e_shnum} sections)")
        shdrs = []
        for i in range(e_shnum):
            sh = _need(data, e_shoff + i * 40, 40, f"section header {i}")
            shdrs.append(struct.unpack("<10I", sh))
        str_type, str_off, str_size = shdrs[e_shstrndx][1], shdrs[e_shstrndx][4], shdrs[e_shstrndx][5]
        if str_type != 3:  # SHT_STRTAB
            raise ImportTableError("shstrtab section is not SHT_STRTAB")
        strtab = _need(data, str_off, str_size, "section name string table")
        for i, sh in enumerate(shdrs):
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = sh[0], sh[1], sh[2], sh[3], sh[4], sh[5]
            # SHT_NOBITS (8) occupies no file bytes; every other section with a
            # size must be fully file-backed, as in tools/elf_bounds.py.
            if sh_type != 8 and sh_size:
                if sh_offset + sh_size > len(data):
                    raise ImportTableError(
                        f"section header {i}: file range {sh_offset:#x}+{sh_size:#x} "
                        f"exceeds file size {len(data):#x}"
                    )
            elif sh_offset > len(data):
                raise ImportTableError(f"section header {i}: offset {sh_offset:#x} is outside the file")
            if sh_name >= len(strtab) and sh_size:
                raise ImportTableError(f"section header {i}: name offset {sh_name:#x} outside shstrtab")
            end = strtab.find(b"\0", sh_name)
            name = strtab[sh_name : end if end >= 0 else len(strtab)]
            # The first section of a name wins, as in tools/analyze.py Elf.sec(); a
            # relocated PRX can carry a second copy of a pairing section.
            if name:
                sections.setdefault(name, (sh_addr, sh_size))
            if name == MODULE_INFO_SECTION and modinfo_vaddr is None:
                if sh_size < MODULE_INFO_SIZE:
                    raise ImportTableError(
                        f"{MODULE_INFO_SECTION.decode()} is {sh_size} bytes; need {MODULE_INFO_SIZE}"
                    )
                modinfo_vaddr = sh_addr
            # SHT_PROGBITS with SHF_ALLOC backs guest ranges when there are no
            # program headers (relocatable PRX fixtures / stripped inputs).
            if not e_phnum and sh_type == 1 and (sh_flags & 2):
                gmap.add(sh_addr, sh_size, sh_offset, len(data), f"section header {i}")

    if not gmap.ranges:
        raise ImportTableError("no PT_LOAD segments or allocatable PROGBITS sections to map guest memory")
    return gmap, modinfo_vaddr, phdr0_paddr, sections


def _span_file_backed(gmap: _GuestMap, start: int, end: int) -> bool:
    """True when [start, end) is empty at (0, 0) or lies inside one loaded range."""
    if start == 0 and end == 0:
        return True
    if end < start or end > U32_MAX + 1:
        return False
    return any(base <= start and end <= base + size for base, size, _off in gmap.ranges)


def _is_structural_module_info(gmap: _GuestMap, record: bytes) -> bool:
    """The record check the analyzer applies to a p_paddr-located SceModuleInfo.

    Mirrors tools/analyze.py _decode_module_info: a non-empty, NUL-terminated,
    printable-ASCII name inside its 28-byte field, word-aligned export and import
    tables, and table spans that are empty or fully file-backed.
    """
    name_field = record[4:4 + MODULE_NAME_BYTES]
    name_len = name_field.find(b"\x00")
    if name_len <= 0 or not all(0x20 <= c < 0x7F for c in name_field[:name_len]):
        return False
    _gp, ent, entend, stub, stubend = struct.unpack_from("<5I", record, 32)
    if ent % 4 or stub % 4:
        return False
    return _span_file_backed(gmap, ent, entend) and _span_file_backed(gmap, stub, stubend)


def _find_module_info(
    data: bytes, gmap: _GuestMap, modinfo_vaddr: int | None, phdr0_paddr: int | None
) -> bytes | None:
    """Return the 52-byte SceModuleInfo record, or None when the input declares none.

    Sectioned inputs name it via .rodata.sceModuleInfo. Stripped PRX/ELF
    inputs use the PRX loader convention instead: phdr[0].p_paddr with the
    kernel-mode bit (bit 31) masked off is the record's file offset. The
    offset must land inside a mapped load range so a forged p_paddr cannot
    reach arbitrary file bytes outside guest-visible data. An input with
    neither a module-info section nor a nonzero phdr[0].p_paddr declares no
    record (None); a declared record that cannot be read is an error.
    """
    if modinfo_vaddr is not None:
        return _read_guest(data, gmap, modinfo_vaddr, MODULE_INFO_SIZE, "SceModuleInfo")
    if not phdr0_paddr:
        return None
    file_off = phdr0_paddr & 0x7FFFFFFF
    for _base, size, range_off in gmap.ranges:
        if range_off <= file_off and file_off + MODULE_INFO_SIZE <= range_off + size:
            record = _need(data, file_off, MODULE_INFO_SIZE, "sectionless SceModuleInfo")
            if not _is_structural_module_info(gmap, record):
                raise ImportTableError(
                    f"sectionless SceModuleInfo at file offset {file_off:#x} is not a "
                    "structurally valid record (name, table alignment, or table span)"
                )
            return record
    raise ImportTableError(
        f"sectionless SceModuleInfo file offset {file_off:#x} (from phdr[0].p_paddr "
        f"{phdr0_paddr:#x}) is not inside any loaded file range"
    )


def _locate_module_info(data: bytes, gmap: _GuestMap, modinfo_vaddr: int | None, phdr0_paddr: int | None) -> bytes:
    """Return the 52-byte SceModuleInfo record; an input without one is an error."""
    record = _find_module_info(data, gmap, modinfo_vaddr, phdr0_paddr)
    if record is None:
        raise ImportTableError(
            f"no {MODULE_INFO_SECTION.decode()} section and phdr[0].p_paddr is absent/zero; "
            "cannot locate SceModuleInfo"
        )
    return record


def parse_export_table(data: bytes) -> ExportTable | None:
    """Parse the module attributes and every exported library out of a PSP ELF.

    Returns None when the input declares no SceModuleInfo record at all (see
    _find_module_info): such an image exports nothing and carries no
    kernel-mode attribute. Every declared table is read in full or rejected
    with ImportTableError, including each entry's guest address table, which
    the loader reads alongside the NIDs.
    """
    gmap, modinfo_vaddr, phdr0_paddr, _sections = _parse_elf_maps(data)
    mi = _find_module_info(data, gmap, modinfo_vaddr, phdr0_paddr)
    if mi is None:
        return None
    (module_attributes,) = struct.unpack_from("<H", mi, 0)
    ent_top, ent_end = struct.unpack_from("<II", mi, 36)
    if ent_top > ent_end:
        raise ImportTableError(f"ent_top {ent_top:#x} is above ent_end {ent_end:#x}")
    if ent_end - ent_top > MAX_EXPORT_TABLE_BYTES:
        raise ImportTableError(
            f"export table spans {ent_end - ent_top:#x} bytes; exceeds the "
            f"{MAX_EXPORT_TABLE_BYTES:#x}-byte loader bound"
        )

    libraries: list[ExportedLibrary] = []
    pos = ent_top
    while pos < ent_end:
        what = f"export entry {len(libraries)} at {pos:#x}"
        if ent_end - pos < EXPORT_ENTRY_HEADER_SIZE:
            raise ImportTableError(
                f"{what}: truncated ({ent_end - pos} bytes left, need {EXPORT_ENTRY_HEADER_SIZE})"
            )
        header = _read_guest(data, gmap, pos, EXPORT_ENTRY_HEADER_SIZE, what)
        name_ptr, _version, attributes, size_words, num_vars, num_funcs, entry_table = struct.unpack(
            "<IHHBBHI", header
        )
        if size_words < MIN_EXPORT_ENTRY_WORDS or size_words > MAX_EXPORT_ENTRY_WORDS:
            raise ImportTableError(
                f"{what}: entry size {size_words} words outside "
                f"[{MIN_EXPORT_ENTRY_WORDS}, {MAX_EXPORT_ENTRY_WORDS}]"
            )
        next_pos = pos + size_words * 4
        if next_pos > ent_end:
            raise ImportTableError(f"{what}: entry size {size_words} words runs past ent_end {ent_end:#x}")
        if len(libraries) >= MAX_LIBRARIES:
            raise ImportTableError(f"more than {MAX_LIBRARIES} export entries")
        count = num_funcs + num_vars
        if count > MAX_FUNCS_PER_LIB:
            raise ImportTableError(f"{what}: {count} exports exceeds cap {MAX_FUNCS_PER_LIB}")
        name = _read_guest_cstr(data, gmap, name_ptr, f"{what} library name") if name_ptr else None
        nids: tuple[int, ...] = ()
        if count:
            if entry_table == 0:
                raise ImportTableError(f"{what}: {count} exports but null entry table pointer")
            nid_blob = _read_guest(data, gmap, entry_table, count * 4, f"{what} NID table")
            _read_guest(data, gmap, entry_table + count * 4, count * 4, f"{what} address table")
            nids = struct.unpack_from(f"<{count}I", nid_blob)
        libraries.append(ExportedLibrary(name, attributes, nids[:num_funcs], nids[num_funcs:]))
        pos = next_pos
    return ExportTable(module_attributes, tuple(libraries))


def variable_imports_message(tables: Sequence[tuple[str, int]]) -> str:
    """The analyzer's named boundary text for a table that declares variables."""
    total = sum(count for _library, count in tables)
    summary = ", ".join(f"{library} x{count}" for library, count in tables)
    return (
        f"import table declares {total} variable imports ({summary}); "
        "variable imports are not supported yet"
    )


class ImportWindow(NamedTuple):
    """One PspLibStubEntry window: a library's run of numFuncs stub slots.

    library is None for a window that imports nothing (its name is never read).
    """

    library: str | None
    count: int  # numFuncs
    nid_data: int  # nidData: the first NID of the run
    first_sym: int  # firstSym: the first stub slot of the run


@dataclass(frozen=True)
class _PairingRegion:
    """One psp-fixup-imports pairing region: stub slots and NIDs that pair 1:1."""

    primary: bool
    windows: tuple[ImportWindow, ...]
    stub_base: int
    stub_end: int
    nid_base: int
    nid_end: int
    finding: str | None


def _section_span(section: tuple[int, int] | None, unit: int, name: str) -> tuple[int, int] | None:
    if section is None:
        return None
    address, size = section
    if size % unit:
        raise ImportTableError(f"{name} size {size} is not a multiple of {unit}")
    return address, address + size


def _spans_overlap(a: tuple[int, int], b: tuple[int, int] | None) -> bool:
    """True when half-open span ``a`` shares an address with ``b`` (``b`` may be None)."""
    return b is not None and a[0] < b[1] and b[0] < a[1]


def _byte_count(nbytes: int, unit: int, label: str) -> str:
    """Name ``nbytes`` in whole ``label`` units of ``unit`` bytes, else in bytes."""
    if nbytes % unit:
        return f"{nbytes} bytes"
    count = nbytes // unit
    return f"{count} {label}{'' if count == 1 else 's'}"


def _window_region(windows: Sequence[ImportWindow]) -> _PairingRegion:
    """A pairing region bounded by the union of its windows' runs."""
    return _PairingRegion(
        primary=False,
        windows=tuple(windows),
        stub_base=min(w.first_sym for w in windows),
        stub_end=max(w.first_sym + w.count * 8 for w in windows),
        nid_base=min(w.nid_data for w in windows),
        nid_end=max(w.nid_data + w.count * 4 for w in windows),
        finding=None,
    )


def _primary_region(
    windows: Sequence[ImportWindow],
    section_stub: tuple[int, int] | None,
    section_nid: tuple[int, int] | None,
) -> _PairingRegion:
    """The psp-fixup-imports pairing region and the windows in it.

    Prefer the named sections; fall back to the union of the windows' runs for a
    section the input does not name.
    """
    window = _window_region(windows)
    stub_base, stub_end = section_stub or (window.stub_base, window.stub_end)
    nid_base, nid_end = section_nid or (window.nid_base, window.nid_end)
    finding = None

    # When the named sections do not pair 1:1, only the windows' own runs can be
    # paired. The two sections then differ in kind:
    #   * .rodata.sceNid words outside the window run are data the loader never
    #     reads, so unreferenced words may sit before it (a head) or after it (a
    #     tail); retail executables carry zero-filled ones on both sides.
    #   * .sceStub.text slots are code. A slot no window claims is a possible call
    #     target that the loader never patches, and without a 1:1 section pairing
    #     it has no NID, so any unreferenced stub slot on either side fails closed.
    if stub_end - stub_base != 2 * (nid_end - nid_base):
        if (window.stub_end - window.stub_base != 2 * (window.nid_end - window.nid_base)
                or not stub_base <= window.stub_base <= window.stub_end <= stub_end
                or not nid_base <= window.nid_base <= window.nid_end <= nid_end
                or (window.nid_base - nid_base) % 4):
            raise ImportTableError(
                "import stub region size does not match NID region size "
                "(psp-fixup-imports requires stub slots to pair 1:1 with NIDs)",
                code=CODE_REGIONS_MISMATCH,
            )
        stub_head, stub_tail = window.stub_base - stub_base, stub_end - window.stub_end
        if stub_head or stub_tail:
            raise ImportTableError(
                f".sceStub.text has {_byte_count(stub_head, 8, 'stub slot')} before and "
                f"{_byte_count(stub_tail, 8, 'stub slot')} after the import windows that no "
                "window claims, and .rodata.sceNid does not pair 1:1 with it, so those "
                "slots have no NID",
                code=CODE_REGIONS_MISMATCH,
            )
        nid_head, nid_tail = window.nid_base - nid_base, nid_end - window.nid_end
        parts = []
        if nid_head:
            parts.append(f"head of {_byte_count(nid_head, 4, 'NID word')}")
        if nid_tail:
            parts.append(f"tail of {_byte_count(nid_tail, 4, 'NID word')}")
        finding = (
            f".rodata.sceNid has an unreferenced {' and a '.join(parts)}; using the "
            f"window-paired run ({(window.stub_end - window.stub_base) // 8} slots)"
        )
        stub_base, stub_end = window.stub_base, window.stub_end
        nid_base, nid_end = window.nid_base, window.nid_end
    return _PairingRegion(
        primary=True,
        windows=tuple(windows),
        stub_base=stub_base,
        stub_end=stub_end,
        nid_base=nid_base,
        nid_end=nid_end,
        finding=finding,
    )


def _pairing_groups(windows: Sequence[ImportWindow]) -> list[list[ImportWindow]]:
    """Group windows by pairing offset, in first-appearance order.

    Inside one pairing region stub slot p sits at stub_base + 8*p and its NID at
    nid_base + 4*p, so firstSym - 2*nidData is the same for every window of the
    region; windows with different offsets can never share a region.
    """
    groups: dict[int, list[ImportWindow]] = {}
    for w in windows:
        groups.setdefault(w.first_sym - 2 * w.nid_data, []).append(w)
    return list(groups.values())


def _contiguous_runs(windows: Sequence[ImportWindow]) -> list[list[ImportWindow]]:
    """Split same-offset windows into runs whose stub slots touch or overlap.

    Each run keeps its windows in table order, so the last claimer of a slot
    stays the one later in the table.
    """
    runs: list[list] = []  # [[table indices], stub end]
    # Visit windows by first stub slot, so a window either overlaps or touches the
    # run being built (extend it to the furthest end seen) or starts a new run
    # after a gap. Ties keep table order (sorted() is stable); a tie cannot change
    # the runs because each run's end is the maximum over its windows.
    order = sorted(range(len(windows)), key=lambda i: windows[i].first_sym)
    for i in order:
        w = windows[i]
        if runs and w.first_sym <= runs[-1][1]:
            runs[-1][0].append(i)
            runs[-1][1] = max(runs[-1][1], w.first_sym + w.count * 8)
        else:
            runs.append([[i], w.first_sym + w.count * 8])
    return [[windows[i] for i in sorted(indices)] for indices, _end in runs]


def _pair_region(
    region: _PairingRegion,
    read_nids: Callable[[int, int], Sequence[int]],
    prefix: str,
) -> tuple[dict[int, tuple[str, int]], list[str]]:
    """Return ({stub address: (library, NID)}, findings) for one pairing region."""
    stub_base, stub_end = region.stub_base, region.stub_end
    nid_base, nid_end = region.nid_base, region.nid_end
    stub_count = (stub_end - stub_base) // 8
    nid_count = (nid_end - nid_base) // 4
    if stub_count != nid_count or nid_count <= 0:
        raise ImportTableError(f"impossible import region: {stub_count} stub slots vs {nid_count} NIDs")

    nids = tuple(read_nids(nid_base, nid_count))
    if len(nids) != nid_count:
        raise ImportTableError(f"truncated import NID region at 0x{nid_base:08x}")

    claims: dict[int, str] = {}  # position -> library name (last claimer wins)
    claimers: dict[int, list[str]] = {}  # position -> libraries in table order
    for w in region.windows:
        if w.first_sym % 4:
            raise ImportTableError(f"import stub area 0x{w.first_sym:08x} is not 4-byte aligned")
        if w.first_sym + w.count * 8 > U32_MAX or w.nid_data + w.count * 4 > U32_MAX:
            raise ImportTableError("import table address arithmetic exceeds 32-bit guest space")
        if w.first_sym < stub_base or (w.first_sym - stub_base) % 8:
            raise ImportTableError(
                f"import stub address 0x{w.first_sym:08x} is not an 8-byte slot of the stub region")
        if w.nid_data < nid_base or (w.nid_data - nid_base) % 4:
            raise ImportTableError(
                f"import NID table 0x{w.nid_data:08x} is not a 4-byte slot of the NID region")
        first_pos = (w.first_sym - stub_base) // 8
        nid_pos = (w.nid_data - nid_base) // 4
        if first_pos != nid_pos:
            raise ImportTableError(
                f"inconsistent import window {w.library}: stub slot {first_pos} "
                f"but NID slot {nid_pos}")
        if first_pos + w.count > stub_count:
            raise ImportTableError(
                f"import window {w.library} with {w.count} functions runs past "
                f"the stub region ({stub_count} slots)")
        for i in range(w.count):
            claims[first_pos + i] = w.library
            claimers.setdefault(first_pos + i, []).append(w.library)

    stubs = {stub_base + p * 8: (claims.get(p, UNATTRIBUTED_LIBRARY), nids[p]) for p in range(nid_count)}
    findings = [region.finding] if region.finding else []
    unclaimed = [p for p in range(nid_count) if p not in claims]
    if unclaimed:
        findings.append(
            f"{prefix}stub slots not covered by any library window: {len(unclaimed)} "
            f"positions {unclaimed}")
    ambiguous = sorted(p for p, libs in claimers.items() if len(libs) > 1)
    if ambiguous:
        findings.append(
            f"{prefix}stub slots claimed by multiple library windows: {len(ambiguous)} "
            f"positions {ambiguous}")
    return stubs, findings


def layout_import_windows(
    windows: Sequence[ImportWindow],
    stub_section: tuple[int, int] | None,
    nid_section: tuple[int, int] | None,
    read_nids: Callable[[int, int], Sequence[int]],
    *,
    max_positions: int | None = None,
) -> tuple[dict[int, tuple[str, int]], list[str]]:
    """Pair every import stub slot with its NID: the layout model both parsers share.

    stub_section and nid_section are the (address, size) of .sceStub.text and
    .rodata.sceNid, or None when the input names no such section. read_nids(address,
    count) returns count NID words from guest memory and raises when they are not
    mapped. Returns ({stub address: (library, NID)}, findings); a slot no window
    claims maps to UNATTRIBUTED_LIBRARY.

    The loader patches only the slots its windows name, and psp-fixup-imports pairs
    the stub and NID sections globally, so:
      * a window whose stub run and NID run both lie outside the named sections is
        detached: windows are grouped by pairing offset and each contiguous run is
        paired by its own slots; overlapping runs with different offsets fail closed;
      * the primary region pairs the sections 1:1 when their sizes allow, and
        otherwise pairs the windows' own runs: unreferenced NID words before or after
        them are accepted (and reported), unreferenced stub slots fail closed;
      * without named sections the single region is the union of the window runs.

    Raises ImportTableError; the boundary codes are the analyzer's.
    """
    if not windows:
        if (stub_section is not None and stub_section[1]) or (nid_section is not None and nid_section[1]):
            raise ImportTableError("import stub table is empty")
        return {}, ["module declares an empty import table"]
    function_windows = [w for w in windows if w.count > 0]
    if not function_windows:
        raise ImportTableError("import stub table has no function windows", code=CODE_TABLE_INVALID)

    section_stub = _section_span(stub_section, 8, ".sceStub.text")
    section_nid = _section_span(nid_section, 4, ".rodata.sceNid")
    primary: list[ImportWindow] = function_windows
    detached: list[ImportWindow] = []
    if section_stub is not None or section_nid is not None:
        primary = []
        for w in function_windows:
            stub_run = (w.first_sym, w.first_sym + w.count * 8)
            nid_run = (w.nid_data, w.nid_data + w.count * 4)
            if _spans_overlap(stub_run, section_stub) or _spans_overlap(nid_run, section_nid):
                primary.append(w)
            else:
                detached.append(w)
        if not primary:
            raise ImportTableError(
                "no import window lies in the named import sections", code=CODE_TABLE_INVALID)

    regions = [_primary_region(primary, section_stub, section_nid)]
    for group in _pairing_groups(detached):
        for run in _contiguous_runs(group):
            regions.append(_window_region(run))

    # Two pairing regions with different offsets must never claim one stub slot:
    # the slot would pair with two different NIDs.
    ordered = sorted(regions, key=lambda r: (r.stub_base, r.stub_end))
    for left, right in zip(ordered, ordered[1:], strict=False):
        if right.stub_base < left.stub_end:
            raise ImportTableError(
                f"import stub runs at 0x{left.stub_base:08x} and 0x{right.stub_base:08x} overlap "
                "but pair with different NID positions",
                code=CODE_REGIONS_MISMATCH,
            )
    positions = sum((r.stub_end - r.stub_base) // 8 for r in regions)
    if max_positions is not None and positions > max_positions:
        raise ImportTableError(f"more than {max_positions} imported functions")

    stubs: dict[int, tuple[str, int]] = {}
    findings: list[str] = []
    for region in regions:
        prefix = "" if region.primary else f"import run at 0x{region.stub_base:08x}: "
        region_stubs, region_findings = _pair_region(region, read_nids, prefix)
        stubs.update(region_stubs)
        findings.extend(region_findings)
    if detached:
        findings.append(
            "import windows outside the named import sections: "
            f"{len(detached)} windows, "
            f"{sum((r.stub_end - r.stub_base) // 8 for r in regions[1:])} slots "
            "paired by their own runs")
    return stubs, findings


def parse_import_table(data: bytes) -> ImportTable:
    """Parse (library, NID, stub address) triples out of a PSP ELF byte buffer."""
    gmap, modinfo_vaddr, phdr0_paddr, sections = _parse_elf_maps(data)
    mi = _locate_module_info(data, gmap, modinfo_vaddr, phdr0_paddr)
    libstub, libstubend = struct.unpack_from("<II", mi, 44)
    if libstub > libstubend:
        raise ImportTableError(f"libstub {libstub:#x} is above libstubend {libstubend:#x}")
    if libstubend - libstub > MAX_LIBRARIES * MAX_STUB_ENTRY_WORDS * 4:
        raise ImportTableError(
            f"libstub table spans {libstubend - libstub:#x} bytes; exceeds defensive cap"
        )

    table = ImportTable(module_attributes=struct.unpack_from("<H", mi, 0)[0])
    seen_libs: set[str] = set()
    windows: list[ImportWindow] = []
    variable_tables: list[tuple[str, int]] = []  # (library, count) per declaring window
    pos = libstub
    entry_index = 0
    while pos < libstubend:
        what = f"import entry {entry_index} at {pos:#x}"
        if libstubend - pos < STUB_ENTRY_HEADER_SIZE:
            raise ImportTableError(f"{what}: truncated ({libstubend - pos} bytes left, need {STUB_ENTRY_HEADER_SIZE})")
        e = _read_guest(data, gmap, pos, STUB_ENTRY_HEADER_SIZE, what)
        name_ptr, _ver, _flags, size_words, num_vars, num_funcs, nid_data, first_sym = struct.unpack(
            "<IHHBBHII", e
        )
        if size_words < MIN_STUB_ENTRY_WORDS or size_words > MAX_STUB_ENTRY_WORDS:
            raise ImportTableError(
                f"{what}: entry size {size_words} words outside "
                f"[{MIN_STUB_ENTRY_WORDS}, {MAX_STUB_ENTRY_WORDS}]"
            )
        next_pos = pos + size_words * 4
        if next_pos > libstubend:
            raise ImportTableError(f"{what}: entry size {size_words} words runs past libstubend {libstubend:#x}")
        # A window that imports nothing claims no stubs and the loader never reads
        # its name, so the name is read only for a window that imports something.
        libname = None
        if num_funcs or num_vars:
            if name_ptr == 0:
                raise ImportTableError(
                    f"{what}: null library name pointer", code=CODE_LIBRARY_NAME_UNMAPPED)
            libname = _read_guest_cstr(data, gmap, name_ptr, f"{what} library name")
            # Duplicate library entries are tolerated (some linkers emit split
            # blocks for one library); duplicate NIDs are surfaced by the auditor.
            seen_libs.add(libname)
            if len(seen_libs) > MAX_LIBRARIES:
                raise ImportTableError(f"more than {MAX_LIBRARIES} import libraries")
        if num_funcs > MAX_FUNCS_PER_LIB:
            raise ImportTableError(f"{what}: {num_funcs} functions exceeds cap {MAX_FUNCS_PER_LIB}")
        if num_funcs and nid_data == 0:
            raise ImportTableError(
                f"{what}: {num_funcs} functions but null NID table pointer", code=CODE_NID_TABLE_MISSING)
        if num_vars:
            # SceLibraryStubTable.vstubtable follows the function-stub pointer, so a
            # declaring entry must be six words long and point at a mapped table.
            if size_words < 6:
                raise ImportTableError(
                    f"{what} declares {num_vars} variables but its {size_words}-word entry "
                    "has no variable-stub table", code=CODE_TABLE_INVALID)
            (vstub_table,) = struct.unpack("<I", _read_guest(data, gmap, pos + 20, 4, what))
            if vstub_table == 0 or vstub_table % 4:
                raise ImportTableError(
                    f"{what} declares {num_vars} variables but its variable-stub table "
                    f"pointer 0x{vstub_table:08x} is null or misaligned", code=CODE_TABLE_INVALID)
            _read_guest(data, gmap, vstub_table, 4, f"{what} variable-stub table")
            variable_tables.append((libname, num_vars))
        if num_funcs:
            # Per-window pointer validation. The stub span check (first_sym ..
            # first_sym + num_funcs*8) requires 4-byte alignment, no 32-bit
            # wrap, and one fully file-backed mapped range (stubs are code; a
            # partially mapped span means a truncated or forged table).
            _read_guest(data, gmap, nid_data, num_funcs * 4, f"{what} NID table")
            if first_sym % 4:
                raise ImportTableError(f"{what}: stub area {first_sym:#x} is not 4-byte aligned")
            if first_sym + num_funcs * 8 > U32_MAX + 1:
                raise ImportTableError(f"{what}: stub area {first_sym:#x} wraps the 32-bit space")
            gmap.to_off(first_sym, num_funcs * 8, f"{what} function stub span")
            table.libraries.append(libname)
        windows.append(ImportWindow(libname, num_funcs, nid_data, first_sym))
        pos = next_pos
        entry_index += 1

    # The analyzer refuses a table that declares variables at its named boundary;
    # the audit refuses it the same way rather than report functions alone.
    if variable_tables:
        raise ImportTableError(
            variable_imports_message(variable_tables), code=CODE_VARIABLE_IMPORTS_UNSUPPORTED)

    def read_nids(address: int, count: int) -> tuple[int, ...]:
        blob = _read_guest(data, gmap, address, count * 4, "global NID region")
        return struct.unpack_from(f"<{count}I", blob)

    stubs, findings = layout_import_windows(
        windows,
        sections.get(STUB_SECTION),
        sections.get(NID_SECTION),
        read_nids,
        max_positions=MAX_TOTAL_FUNCS,
    )
    table.funcs = [
        ImportedFunc(library, nid, stub_addr)
        for stub_addr, (library, nid) in sorted(stubs.items())
    ]
    table.findings = findings
    return table
