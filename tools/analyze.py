# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Function-boundary analyzer for PSP ELF/PRX modules (Phase 2).
#
# Discovers function entry points without using the symbol table, by combining:
#   - the ELF entry point and the module's start/stop exports,
#   - direct-call targets (jal) found while tracing reachable instructions,
#   - constructor/destructor pointer arrays (.ctors/.dtors),
#   - function-pointer tables in read-only/data sections (pointers into code),
#   - recursive descent from each entry to bound the function and find more calls.
#
# When the module has a symbol table (the homebrew does), it reports recall against the
# STT_FUNC symbols. It also emits a TOML function inventory.
#
# Usage: analyze.py <elf> [--toml out.toml] [--quiet]

import bisect
import os
import struct
import sys

import tomllib

from elf_bounds import MAX_ELF_FILE_BYTES, validate_elf32_envelope
from analyze_cfg import (  # noqa: F401  (re-exported: codegen, tests and tools use analyze.<name>)
    CFG_SCHEMA_VERSION,
    CFG_SUCCESSOR_CLASSIFICATIONS,
    CanonicalCfgState,
    REGIMM_BRANCH_RT,
    STATIC_COVERAGE_SCHEMA_VERSION,
    UINT32_END_MAX,
    _CFG_DETAIL_IDS,
    _CFG_DETAIL_NAMES,
    _CFG_KIND_IDS,
    _CFG_KIND_NAMES,
    _CFG_NONE,
    _CFG_REASON_BITS,
    _CFG_REASON_NAMES,
    _cfg_address_comparable,
    _cfg_branch_likely,
    _cfg_control_transfer,
    _cfg_field,
    _cfg_is_eret,
    _cfg_normalize_ranges,
    _cfg_opcode_identity,
    _cfg_ranges_for,
    _cfg_section_blob,
    _cfg_successor_classifications,
    canonical_cfg_gate,
    canonical_cfg_json,
    canonical_cfg_report,
    canonical_cfg_state,
    canonical_cfg_summary,
    cfg_compatibility_findings,
    section_bytes,
    static_coverage,
    verify_canonical_cfg_report,
    verify_canonical_cfg_state,
)

# Input classes supported by the Prx rebase/relocate path. Only genuinely
# relocatable images are routed there: PSP PRX-format ELFs (ET_SCE_PRX),
# relocatable ELFs (ET_REL), and executables that still carry relocation
# sections (SHT_RELA/SHT_REL, or the PSP types SHT_PSP_RELA/SHT_PSP_REL --
# e.g. the -Wl,-q PSPDEV ET_EXEC form, whose link-time addresses are
# 0-based and must be rebased). Ordinary ET_EXEC/ET_DYN ELFs without
# relocation sections at a nonzero base are handled as-is: their sections
# already carry concrete addresses.
ET_REL = 1
ET_EXEC = 2
ET_SCE_PRX = 0xFFA0
SHT_RELA = 4
SHT_REL = 9
SHT_PSP_RELA = 0x700000A0
SHT_PSP_REL = 0x700000A1

# SceModuleInfo: u16 attribute, u16 version, char name[28], then gp, ent_top,
# ent_end, stub_top, stub_end (five u32 words).
MODULE_INFO_SIZE = 52
MODULE_INFO_NAME_BYTES = 28
# Bit 31 of the first loadable segment's p_paddr marks a kernel-mode module;
# the remaining bits are the SceModuleInfo file offset.
PSP_MODULE_INFO_KERNEL_BIT = 0x80000000
# PspModuleInfoAttr bits PSPSDK pspmoduleinfo.h documents: NO_STOP (0x0001),
# SINGLE_LOAD (0x0002), SINGLE_START (0x0004) and KERNEL (0x1000).
PSP_MODULE_INFO_ATTRIBUTES = 0x1007
# Shortest library records, in 32-bit words: a SceLibraryEntryTable carries a
# name, version/attribute, length/counts and its entry-table pointer; a
# SceLibraryStubTable adds the NID-table and stub-table pointers.
LIBRARY_ENTRY_MIN_WORDS = 4
LIBRARY_STUB_MIN_WORDS = 5


def _file_backed_guest_span(segments, start, end):
    """True when [start, end) is empty (0, 0) or inside one PT_LOAD's file bytes."""
    if start == 0 and end == 0:
        return True
    if end < start or end > 0x100000000:
        return False
    return any(
        segment["type"] == 1
        and segment["vaddr"] <= start
        and end <= segment["vaddr"] + segment["filesz"]
        and segment["vaddr"] + segment["filesz"] <= 0x100000000
        for segment in segments
    )


def _decode_module_info(segments, seg_data, o):
    """Decode a candidate SceModuleInfo at ``seg_data[o:]`` or return None.

    A record is structurally valid when its name is a non-empty, terminated,
    printable-ASCII string, and its export and import table spans are either
    empty or word-aligned and fully file-backed.
    """
    if o < 0 or o + MODULE_INFO_SIZE > len(seg_data):
        return None
    name_bytes = seg_data[o + 4:o + 4 + MODULE_INFO_NAME_BYTES]
    name_len = name_bytes.find(b"\x00")
    if name_len <= 0 or not all(32 <= c < 127 for c in name_bytes[:name_len]):
        return None
    attribute = struct.unpack_from("<H", seg_data, o)[0]
    gp, ent, entend, stub, stubend = struct.unpack("<5I", seg_data[o + 32:o + 52])
    if ent % 4 or stub % 4:
        return None
    if not (_file_backed_guest_span(segments, ent, entend)
            and _file_backed_guest_span(segments, stub, stubend)):
        return None
    return dict(name=name_bytes[:name_len].decode("ascii"),
                name_padded=not any(name_bytes[name_len:]), attribute=attribute, gp=gp,
                ent=ent, entend=entend, stub=stub, stubend=stubend)


def _file_bytes(segments, data, start, end):
    """File bytes backing guest span [start, end) inside one PT_LOAD, or None."""
    for segment in segments:
        if (segment["type"] == 1 and segment["vaddr"] <= start
                and end <= segment["vaddr"] + segment["filesz"]):
            off = segment["off"] + (start - segment["vaddr"])
            return data[off:off + (end - start)]
    return None


def _library_table_walks(segments, data, top, end, min_words):
    """True when [top, end) splits exactly into library records.

    Each record's length byte (offset 8, in words) must be at least
    ``min_words`` and the last record must end exactly at ``end``. An empty
    span is a table without records.
    """
    if top == end:
        return True
    blob = _file_bytes(segments, data, top, end)
    if blob is None:
        return False
    pos = 0
    while pos < len(blob):
        if pos + 12 > len(blob) or blob[pos + 8] < min_words:
            return False
        pos += blob[pos + 8] * 4
    return pos == len(blob)


def _module_info_is_consistent(segments, data, record):
    """True when a decoded SceModuleInfo proves its own internal consistency.

    The name is NUL-padded to the end of its field, the attribute uses only
    documented PspModuleInfoAttr bits, and the export and import spans split
    exactly into whole library records, at least one of them non-empty. gp is
    deliberately not a criterion: a module built without $gp-relative data
    carries gp == 0, and short module names are legal too.
    """
    if not record["name_padded"] or record["attribute"] & ~PSP_MODULE_INFO_ATTRIBUTES:
        return False
    ent, entend = record["ent"], record["entend"]
    stub, stubend = record["stub"], record["stubend"]
    if ent == entend and stub == stubend:
        return False
    return (_library_table_walks(segments, data, ent, entend, LIBRARY_ENTRY_MIN_WORDS)
            and _library_table_walks(segments, data, stub, stubend, LIBRARY_STUB_MIN_WORDS))


def _module_info_from_load_paddr(segments, code_seg, seg_data, load_paddr):
    """Locate SceModuleInfo through the PSP executable convention.

    The PSP loader finds a module's SceModuleInfo through the first loadable
    segment: its p_paddr, with the kernel-mode bit masked off, is the record's
    file offset (docs/cleanroom/PRX_LOADER_SPEC.md section 3.4). Return
    ``(offset in code_seg, ent, entend, stub, stubend)`` when that offset lies
    inside the code segment's file bytes and decodes to a structurally valid
    record; otherwise None (for example a toolchain ELF whose p_paddr simply
    repeats p_vaddr). This is the loader's own designation, so the record only
    has to decode; the internal-consistency proof is for the fallback scan.
    """
    if not load_paddr:
        return None
    file_off = load_paddr & ~PSP_MODULE_INFO_KERNEL_BIT & 0xFFFFFFFF
    o = file_off - code_seg["off"]
    record = _decode_module_info(segments, seg_data, o)
    if record is None:
        return None
    return o, record["ent"], record["entend"], record["stub"], record["stubend"]


def _scan_module_info(segments, data, seg_data):
    """Fallback: the first internally consistent SceModuleInfo in the code segment.

    Used only when p_paddr does not name a valid record. Without the loader's
    designation a candidate must prove itself (_module_info_is_consistent), so
    a printable string followed by plausible pointers is not taken for the
    record, while real records with gp == 0 or short names are still found.
    """
    for o in range(0, len(seg_data) - MODULE_INFO_SIZE, 4):
        record = _decode_module_info(segments, seg_data, o)
        if record is None or not _module_info_is_consistent(segments, data, record):
            continue
        return o, record["ent"], record["entend"], record["stub"], record["stubend"]
    return None


# Metrowerks CodeWarrior code overlays ("MWo3"). The game reads one whole file to a fixed
# address itself, then announces the new code with an instruction-cache invalidate; it is
# not a module and the kernel never sees it. Layout, read from the 13 overlays of one title
# and consistent across all of them: a 0x40-byte header (magic, overlay id, load address,
# text size, data size, bss size, init-array start, init-array end, then the file name),
# the text right after the header, the data after the text (the file ends there), and the
# bss after the file. The init array lies in the data and lists the constructors the game's
# loader runs after it clears the bss.
CODE_OVERLAY_MAGIC = b"MWo3"
CODE_OVERLAY_HEADER_BYTES = 0x40


class CodeOverlayLayout:
    __slots__ = ("load", "text_size", "data_size", "bss_size", "init_lo", "init_hi")

    def __init__(self, load, text_size, data_size, bss_size, init_lo, init_hi):
        self.load = load
        self.text_size = text_size
        self.data_size = data_size
        self.bss_size = bss_size
        self.init_lo = init_lo
        self.init_hi = init_hi

    @property
    def text_lo(self):
        return self.load + CODE_OVERLAY_HEADER_BYTES

    @property
    def data_lo(self):
        return self.text_lo + self.text_size

    @property
    def file_end(self):
        return self.data_lo + self.data_size

    @property
    def check_bytes(self):
        """The bytes that identify the overlay in guest memory: its header and its text.

        The data is left out, because the game writes it once the overlay runs."""
        return CODE_OVERLAY_HEADER_BYTES + self.text_size


def data_section_words(elf):
    """Every aligned word in an image's file-backed data sections (not .text or stubs).

    These are the pointers it holds; codegen hands them to a code overlay's analysis as
    entry hints, because the executable keeps the table of its overlays' entry points."""
    words = set()
    for s in elf.sections:
        if s["typ"] != 1 or not s["size"] or s.get("nm") in (".text", ".sceStub.text"):
            continue
        blob = elf.data[s["off"]:s["off"] + s["size"]]
        words.update(int.from_bytes(blob[o:o + 4], "little") for o in range(0, len(blob) - 3, 4))
    return words


def is_code_overlay(data):
    return bytes(data[:4]) == CODE_OVERLAY_MAGIC


def code_overlay_layout(data):
    """Validate an MWo3 code overlay and return its CodeOverlayLayout; ValueError if malformed."""
    if not is_code_overlay(data):
        raise ValueError("not an MWo3 code overlay")
    if len(data) < CODE_OVERLAY_HEADER_BYTES:
        raise ValueError("MWo3 overlay is shorter than its header")
    load, text_size, data_size, bss_size, init_lo, init_hi = struct.unpack_from("<6I", data, 8)
    layout = CodeOverlayLayout(load, text_size, data_size, bss_size, init_lo, init_hi)
    if load & 3 or text_size & 3 or text_size == 0:
        raise ValueError("MWo3 load address or text size is not word aligned, or the text is empty")
    if CODE_OVERLAY_HEADER_BYTES + text_size + data_size != len(data):
        raise ValueError("MWo3 header sizes do not add up to the file size")
    if layout.file_end + bss_size > 0x100000000:
        raise ValueError("MWo3 image exceeds the 32-bit address space")
    if init_lo & 3 or init_hi & 3 or not layout.data_lo <= init_lo <= init_hi <= layout.file_end:
        raise ValueError("MWo3 init array lies outside the data")
    return layout


def code_overlay_elf(data):
    """A fixed-address ELF32 image of an MWo3 overlay, for the analyzer and code generator.

    One PT_LOAD maps the whole file at its load address (bss included), and the sections
    name the text, data, init array and bss, so the analysis treats the overlay exactly as
    it treats a fixed-address module: the text is the only code, and the init array and
    data pointer tables seed functions. Nothing is relocated."""
    layout = code_overlay_layout(data)
    names = b"\0.text\0.data\0.init_array\0.bss\0.shstrtab\0"
    image_off = 0x60                                    # header (52) + one phdr (32), aligned
    names_off = image_off + len(data)
    shoff = (names_off + len(names) + 3) & ~3

    def name_at(text):
        return names.index(text + b"\0")

    sections = [(0, 0, 0, 0, 0, 0)]
    sections.append((name_at(b".text"), 1, 0x6, layout.text_lo,
                     image_off + CODE_OVERLAY_HEADER_BYTES, layout.text_size))
    if layout.data_size:
        sections.append((name_at(b".data"), 1, 0x3, layout.data_lo,
                         image_off + layout.data_lo - layout.load, layout.data_size))
    if layout.init_hi > layout.init_lo:
        sections.append((name_at(b".init_array"), 14, 0x3, layout.init_lo,
                         image_off + layout.init_lo - layout.load, layout.init_hi - layout.init_lo))
    if layout.bss_size:
        sections.append((name_at(b".bss"), 8, 0x3, layout.file_end, names_off, layout.bss_size))
    sections.append((name_at(b".shstrtab"), 3, 0, 0, names_off, len(names)))

    out = bytearray(shoff + 40 * len(sections))
    out[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 8, 1, 0, 52, shoff, 0,
                     52, 32, 1, 40, len(sections), len(sections) - 1)
    struct.pack_into("<8I", out, 52, 1, image_off, layout.load, 0, len(data),
                     len(data) + layout.bss_size, 7, 4)
    out[image_off:image_off + len(data)] = data
    out[names_off:names_off + len(names)] = names
    for index, (name, typ, flags, addr, off, size) in enumerate(sections):
        struct.pack_into("<10I", out, shoff + 40 * index, name, typ, flags, addr, off, size,
                         0, 0, 4 if typ else 0, 0)
    return bytes(out)


class Elf:
    def __init__(self, path, base=None):
        memory_input = isinstance(path, (bytes, bytearray, memoryview))
        input_label = "<ELF memory buffer>" if memory_input else path
        if memory_input:
            self.data = bytes(path)
        else:
            with open(path, "rb") as source:
                self.data = source.read(MAX_ELF_FILE_BYTES + 1)
        if len(self.data) > MAX_ELF_FILE_BYTES:
            raise ValueError(f"{input_label}: ELF file exceeds the 256 MiB input bound")
        if is_code_overlay(self.data):
            # A code overlay is read through its fixed-address image (code_overlay_elf).
            if base is not None:
                raise ValueError(f"{input_label}: a code overlay loads only at its own address")
            self.data = code_overlay_elf(self.data)
        self.base = base
        d = self.data
        self.reloc = None
        self.relocated_words = None  # _relocated_words
        envelope = validate_elf32_envelope(d, input_label)
        # Section-table defects the PSP loader never sees (tools/elf_bounds.py); the
        # package route reports them as analysis diagnostics.
        self.envelope_findings = list(envelope["findings"])
        self.entry = envelope["entry"]
        self.phoff = envelope["phoff"]
        self.shoff = envelope["shoff"]
        self.phentsize = envelope["phentsize"]
        self.phnum = envelope["phnum"]
        self.shentsize = envelope["shentsize"]
        self.shnum = envelope["shnum"]
        self.shstrndx = envelope["shstrndx"]
        self.segments = [
            dict(
                type=p["type"],
                off=p["off"],
                vaddr=p["vaddr"],
                filesz=p["filesz"],
                memsz=p["memsz"],
                flags=p["flags"],
                idx=p["idx"],
            )
            for p in envelope["phdrs"]
        ]

        self.sections = []
        named_sections_available = False
        if self.shnum > 0 and self.shentsize > 0 and self.shstrndx > 0:
            shstr = envelope["shdrs"][self.shstrndx]
            section_names = d[shstr["off"] : shstr["off"] + shstr["size"]]
            # A section table whose string table names no code section (e.g. a
            # decrypted executable with an emptied .shstrtab) carries no usable
            # layout; reconstruct from PT_LOAD instead.
            named_sections_available = (b".text\x00" in section_names
                                        or b".sceStub.text\x00" in section_names)
        if named_sections_available:
            for section in envelope["shdrs"]:
                name, typ, flags, addr, off, size, link, info, _align, entsz = (
                    section["name"], section["typ"], section["flags"], section["addr"],
                    section["off"], section["size"], section["link"], section["info"],
                    section["align"], section["entsz"],
                )
                self.sections.append(dict(name=name, typ=typ, flags=flags, addr=addr,
                                          off=off, size=size, link=link, info=info, entsz=entsz))
            shstr = self.sections[self.shstrndx]
            for s in self.sections:
                e = shstr["off"] + s["name"]
                s["nm"] = d[e:d.find(b"\x00", e)].decode("ascii", "replace")
        else:
            # Reconstruct sections for stripped PRX
            for seg in self.segments:
                if seg["type"] == 1 and seg["filesz"] > 0:  # PT_LOAD
                    if seg["flags"] & 1:  # Executable
                        self.sections.append(dict(name=0, typ=1, flags=seg["flags"], addr=seg["vaddr"],
                                                  off=seg["off"], size=seg["filesz"], link=0, info=0, entsz=0, nm=".text"))
                    else:  # Data
                        self.sections.append(dict(name=0, typ=1, flags=seg["flags"], addr=seg["vaddr"],
                                                  off=seg["off"], size=seg["filesz"], link=0, info=0, entsz=0, nm=".data"))
            # Locate SceModuleInfo in the first executable PT_LOAD segment.
            code_seg = next((s for s in self.segments if s["type"] == 1 and (s["flags"] & 1)), None)
            if code_seg:
                seg_data = d[code_seg["off"] : code_seg["off"] + code_seg["filesz"]]
                load_paddr = next(
                    (p["paddr"] for p in envelope["phdrs"] if p["type"] == 1), None)
                record = _module_info_from_load_paddr(self.segments, code_seg, seg_data, load_paddr)
                if record is None:
                    record = _scan_module_info(self.segments, d, seg_data)
                if record is not None:
                    self._add_module_metadata_sections(code_seg, *record)

        # PRX (ET_SCE_PRX = 0xFFA0), relocatable (ET_REL), and relocation-bearing
        # ELFs (e.g. the -Wl,-q PSPDEV ET_EXEC form): rebase to `base` and apply
        # the PSP relocations, so the code has concrete addresses. Non-PRX
        # classes are only rebased at a nonzero base (a zero base carries no
        # rebase work and matches the legacy HST-style input handling).
        # Ordinary ET_EXEC/ET_DYN ELFs without relocation sections keep their
        # as-is addressing. After this, read_at_vaddr serves the relocated
        # image and section/segment addresses are in the rebased space.
        e_type = struct.unpack("<H", d[16:18])[0]
        has_reloc_sections = any(
            s["typ"] in (SHT_RELA, SHT_REL, SHT_PSP_RELA, SHT_PSP_REL)
            for s in self.sections
        )
        # A fixed-address ET_EXEC whose base names its lowest PT_LOAD address is
        # already at its guest addresses, the rule prxload's flat image applies
        # (_image_address_bias). Retail executables keep empty .rel.* sections,
        # which made the analyzer rebase such an image a second time and read
        # its module info from an unmapped address.
        fixed_exec_at_base = e_type == 2 and base is not None and base != 0 and any(
            s["type"] == 1 for s in self.segments
        ) and min(s["vaddr"] for s in self.segments if s["type"] == 1) == base
        if base is not None and not fixed_exec_at_base and (
            e_type == ET_SCE_PRX or (base != 0 and (e_type == ET_REL or has_reloc_sections))
        ):
            from prxload import Prx
            prx = Prx(self.data if memory_input else path, base)
            prx.relocate()
            self.reloc = prx
            if e_type == ET_SCE_PRX and prx.relocated:
                self.relocated_words = prx.relocated
            self.entry = (self.entry + base) & 0xFFFFFFFF
            for s in self.sections:
                s["addr"] += base
            for s in self.segments:
                s["vaddr"] += base

    def _add_module_metadata_sections(self, code_seg, o, ent, entend, stub, stubend):
        """Expose a located SceModuleInfo and the tables it names as sections.

        ``o`` is the record's offset inside ``code_seg``. Everything before the
        first metadata table is code (.text and .sceStub.text); everything after
        the last one is read-only data.
        """
        module_info_offset = code_seg["vaddr"] + o
        self.sections.append(dict(name=0, typ=1, flags=2, addr=module_info_offset,
                                  off=code_seg["off"] + o, size=52, link=0, info=0, entsz=0, nm=".rodata.sceModuleInfo"))
        if stubend > stub:
            stub_off = -1
            for s in self.segments:
                if s["vaddr"] <= stub < s["vaddr"] + s["memsz"]:
                    stub_off = s["off"] + (stub - s["vaddr"])
                    break
            if stub_off != -1:
                self.sections.append(dict(name=0, typ=1, flags=6, addr=stub,
                                          off=stub_off, size=stubend - stub, link=0, info=0, entsz=0, nm=".lib.stub"))
        if entend > ent:
            ent_off = -1
            for s in self.segments:
                if s["vaddr"] <= ent < s["vaddr"] + s["memsz"]:
                    ent_off = s["off"] + (ent - s["vaddr"])
                    break
            if ent_off != -1:
                self.sections.append(dict(name=0, typ=1, flags=2, addr=ent,
                                          off=ent_off, size=entend - ent, link=0, info=0, entsz=0, nm=".lib.ent"))

        meta_start = module_info_offset
        if entend > ent and code_seg["vaddr"] <= ent < code_seg["vaddr"] + code_seg["memsz"]:
            meta_start = min(meta_start, ent)
        if stubend > stub and code_seg["vaddr"] <= stub < code_seg["vaddr"] + code_seg["memsz"]:
            meta_start = min(meta_start, stub)
        text_sec = next((s for s in self.sections if s.get("nm") == ".text"), None)
        if text_sec:
            text_sec["size"] = meta_start - text_sec["addr"]
        # Everything in the code segment after the module metadata is
        # read-only data (.rodata.sceResident/.sceNid/.rodata): switch
        # jump tables and function-pointer tables live there, so expose
        # it as .rodata for the data-pointer scan.
        meta_end = module_info_offset + 52
        if entend > ent and code_seg["vaddr"] <= ent < code_seg["vaddr"] + code_seg["memsz"]:
            meta_end = max(meta_end, entend)
        if stubend > stub and code_seg["vaddr"] <= stub < code_seg["vaddr"] + code_seg["memsz"]:
            meta_end = max(meta_end, stubend)
        seg_end = code_seg["vaddr"] + code_seg["filesz"]
        if meta_end < seg_end:
            self.sections.append(dict(name=0, typ=1, flags=2, addr=meta_end,
                                      off=code_seg["off"] + (meta_end - code_seg["vaddr"]),
                                      size=seg_end - meta_end, link=0, info=0, entsz=0,
                                      nm=".rodata"))

    def sec(self, name):
        return next((s for s in self.sections if s.get("nm") == name), None)

    def read_at_vaddr(self, vaddr, n):
        # For a relocated PRX, serve the rebased+relocated image directly.
        if self.reloc is not None:
            if self.reloc.lo <= vaddr < self.reloc.lo + len(self.reloc.mem):
                o = vaddr - self.reloc.lo
                return bytes(self.reloc.mem[o:o + n])
            return None
        # Otherwise read from the loaded image via PT_LOAD program headers. Sections are not
        # used here because non-code allocated sections (.reginfo, .MIPS.abiflags) can share a
        # vaddr with .text and would otherwise shadow the real instruction bytes. The PT_LOAD
        # spans are taken once, in program-header order, so the first match is unchanged;
        # codegen reads millions of instruction words through here.
        loads = getattr(self, "_load_spans", None)
        if loads is None:
            loads = self._load_spans = tuple(
                (seg["vaddr"], seg["vaddr"] + seg["filesz"], seg["off"])
                for seg in self.segments if seg["type"] == 1
            )
        for lo, hi, off in loads:
            if lo <= vaddr < hi:
                start = off + (vaddr - lo)
                return self.data[start:start + n]
        return None

    def func_symbols(self):
        symtab, strtab = self.sec(".symtab"), self.sec(".strtab")
        if not symtab or not strtab:
            return None
        d = self.data
        out = set()
        for i in range(symtab["size"] // symtab["entsz"]):
            o = symtab["off"] + i * symtab["entsz"]
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack("<IIIBBH", d[o:o + 16])
            if (st_info & 0xF) == 2 and st_value != 0:  # STT_FUNC
                out.add(st_value)
        return out


EXEC_SECTIONS = (".text", ".init", ".fini", ".sceStub.text", ".data", ".rodata.sceResident")


EXTRA_SPAN_ENV = "TITLE_EXTRA_SPANS"


def parse_extra_spans(text, source=EXTRA_SPAN_ENV):
    """Parse one explicit ``"lo,hi"`` executable span into ``[(lo, hi)]``.

    An empty/whitespace-only string means *no* extra span. `source` only names the
    origin in error messages (an environment variable or a command-line option), so
    a malformed value fails closed with an actionable message instead of silently
    analyzing the wrong address range.
    """
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    parts = [part.strip() for part in text.split(",")]
    if len(parts) != 2:
        raise RuntimeError("%s must look like 'lo,hi' (got %r)" % (source, text))
    try:
        lo, hi = (int(part, 0) for part in parts)
    except ValueError as exc:
        raise RuntimeError(
            "%s must contain numeric addresses (got %r)" % (source, text)
        ) from exc
    if lo < 0 or hi < 0:
        raise RuntimeError("%s must not contain negative addresses (got %r)" % (source, text))
    if hi <= lo:
        raise RuntimeError("%s requires hi > lo (got %r)" % (source, text))
    if hi > UINT32_END_MAX:
        raise RuntimeError("%s exceeds the 32-bit guest address space (got %r)" % (source, text))
    return [(lo, hi)]


def analyzer_span_from_env(environ=None):
    """Return the explicit extra executable span the caller put in the environment.

    This is the *only* place a span may enter from ambient process state, and it is
    called exclusively from CLI entry points for the primary image. Library callers
    (`exec_ranges`, `analyze`) never consult the environment, so an inherited value
    cannot leak into a rebased extra guest module analyzed in the same process.

    TITLE_EXTRA_SPANS is the sole authority. Retired environment aliases are ignored,
    so stale process state cannot affect a generic build whether TITLE is absent or
    explicitly empty.
    """
    if environ is None:
        environ = os.environ
    if EXTRA_SPAN_ENV in environ:
        return parse_extra_spans(environ.get(EXTRA_SPAN_ENV), EXTRA_SPAN_ENV)
    return None


def resolve_extra_spans(cli_text, environ=None, source="--extra-span"):
    """Resolve the effective span from an explicit option plus the environment.

    An explicit option wins, but a *conflicting* environment value fails closed
    rather than letting the two disagree silently about what was analyzed.
    """
    from_cli = parse_extra_spans(cli_text, source)
    from_env = analyzer_span_from_env(environ)
    if from_cli is None:
        return from_env
    if from_env is not None and from_env != from_cli:
        raise RuntimeError(
            "%s=%r conflicts with %s=%r" % (source, cli_text, EXTRA_SPAN_ENV, from_env)
        )
    return from_cli


def exec_ranges(elf, extra_spans=None):
    # Build the executable address ranges from the section table, restricted to actual code
    # sections. PSP PRX ELFs declare almost every section with SHF_EXECINSTR (the loader
    # maps the whole module flat with WAX), so a naive flag check still treats .data/.rodata
    # tables as code -- the analyzer then sees fake "functions" at every data word and the
    # codegen emits thousands of bogus stubs in .data. Use the section *name* as the source
    # of truth: only .text and its siblings are recompiled. .lib.ent / .lib.stub are linker
    # bookkeeping (import tables / module stubs), filled in by the loader at runtime -- never
    # to be recompiled. .rodata.* / .data are data sections; their word content merely looks
    # like MIPS to a naive decoder.
    spans = []
    for s in elf.sections:
        if s["typ"] != 1:  # only PROGBITS
            continue
        name = s.get("nm")
        if name is None:
            if elf.shnum > 0 and elf.shentsize > 0:
                shstr = elf.sections[elf.shstrndx]
                name = elf.data[shstr["off"] + s["name"]: elf.data.find(b"\x00", shstr["off"] + s["name"])].decode("ascii", "replace")
            else:
                continue
        # Real code is .text and PSP-specific .sceStub.text sections. Everything else
        # (.lib.*, .rodata.*, .data, etc.) is data/linker bookkeeping.
        if name != ".text" and name != ".sceStub.text":
            continue
        if s["addr"] is None or s["size"] is None or s["size"] <= 0:
            continue
        spans.append((s["addr"], s["addr"] + s["size"]))
    # Extra code spans that live outside the section table are *caller-supplied title
    # configuration*, never a built-in default. They are guest addresses. A base-zero
    # image and a fixed-address executable (one GAME_BASE does not move) are analyzed at
    # those addresses, so the span applies as written. A relocated image was moved by
    # GAME_BASE, and a span written for it could name either address space, so that
    # combination still fails closed instead of analyzing the wrong bytes.
    if extra_spans:
        if elf.reloc is not None and elf.base:
            raise RuntimeError(
                "explicit extra executable spans cannot be applied to an image relocated "
                "to GAME_BASE=0x%x" % elf.base
            )
        spans.extend((lo, hi) for lo, hi in extra_spans)
    if not spans:
        return [(0, 0)]
    spans.sort()
    merged = [spans[0]]
    for lo, hi in spans[1:]:
        if lo <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


def in_ranges(addr, ranges):
    if isinstance(ranges, _SpanIndex):
        return addr in ranges
    # A plain loop rather than any(<generator>): the same answer without building a
    # generator per call, and analysis plus codegen make millions of these calls.
    for lo, hi in ranges:
        if lo <= addr < hi:
            return True
    return False


class _SpanIndex:
    """Membership over half-open ``(lo, hi)`` spans, answering exactly as ``in_ranges``.

    The spans are merged once into disjoint, sorted intervals, so a lookup is a
    binary search instead of a scan of every span. Empty spans (``lo >= hi``)
    can contain no address and are dropped; touching spans merge, which leaves
    the covered set of integers unchanged.
    """

    __slots__ = ("starts", "ends", "_key")

    def __init__(self, ranges):
        self.starts = []
        self.ends = []
        self._key = None
        for lo, hi in sorted((lo, hi) for lo, hi in ranges if lo < hi):
            if self.ends and lo <= self.ends[-1]:
                if hi > self.ends[-1]:
                    self.ends[-1] = hi
            else:
                self.starts.append(lo)
                self.ends.append(hi)

    def __contains__(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return i >= 0 and addr < self.ends[i]


def span_index(ranges):
    """``ranges`` (a span list or a :class:`_SpanIndex`) as a :class:`_SpanIndex`.

    Membership in the index is a binary search with the same answer as
    :func:`in_ranges`. A walker that tests one address per instruction builds it
    once per walk: an image whose owned code is split into hundreds of spans (code
    reached outside the named sections becomes one span per run) otherwise pays a
    scan of every span on each test.
    """
    return ranges if isinstance(ranges, _SpanIndex) else _SpanIndex(ranges)


def span_key(ranges):
    """A hashable key for ``ranges`` (a span list or a :class:`_SpanIndex`).

    It is the merged, sorted, non-empty span tuple, so two range lists covering
    the same addresses have the same key, whichever form they arrive in. An index
    computes its key once.
    """
    index = span_index(ranges)
    if index._key is None:
        index._key = tuple(zip(index.starts, index.ends, strict=True))
    return index._key


#: Allocated sections whose name marks them as data, whatever their flags say: PSP PRX
#: images declare almost every section SHF_EXECINSTR (see exec_ranges).
DATA_SECTION_PREFIXES = (
    ".rodata", ".data", ".sdata", ".bss", ".sbss", ".lib.", ".eh_frame",
    ".gcc_except_table", ".ctors", ".dtors", ".init_array", ".fini_array",
    ".preinit_array", ".got", ".reginfo", ".MIPS.",
)
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4


def _section_name(elf, section):
    """A section's name, read through the section-string table when the record has none."""
    name = section.get("nm")
    if name is not None:
        return name
    if elf.shnum <= 0 or elf.shentsize <= 0 or not elf.shstrndx:
        return ""
    shstr = elf.sections[elf.shstrndx]
    start = shstr["off"] + section["name"]
    end = elf.data.find(b"\x00", start, shstr["off"] + shstr["size"])
    return elf.data[start:end].decode("ascii", "replace") if end >= 0 else ""


def _data_section_spans(elf):
    """Guest spans of the allocated sections the section table names as data.

    A data section is allocated and either not SHF_EXECINSTR or named as data. Its
    bytes are data even when the PT_LOAD that maps it is executable: a fixed-address
    executable commonly maps .text, .rodata and .data with one R+X segment.
    """
    spans = []
    for section in getattr(elf, "sections", ()):
        flags = section.get("flags", 0)
        if not (flags & SHF_ALLOC) or not section.get("size") or section.get("addr") is None:
            continue
        name = _section_name(elf, section)
        if (flags & SHF_EXECINSTR) and not name.startswith(DATA_SECTION_PREFIXES):
            continue
        spans.append((section["addr"], section["addr"] + section["size"]))
    return sorted(spans)


def _file_backed_exec_ranges(elf):
    """Executable bytes that are file-backed, including bytes outside named code sections.

    The executable PT_LOAD segments, less the data sections they map. Bytes outside
    every named section (a module-start trampoline before .text, .init/.fini code)
    stay executable; a pointer into .rodata or .data is data, never a code pointer.
    """
    cached = getattr(elf, "_file_backed_exec_ranges", None)
    if cached is None:
        data_spans = _data_section_spans(elf)
        cached = []
        for segment in getattr(elf, "segments", ()):
            if segment["type"] != 1 or not (segment.get("flags", 0) & 1) or segment["filesz"] <= 0:
                continue
            pieces = [(segment["vaddr"], segment["vaddr"] + segment["filesz"])]
            for data_lo, data_hi in data_spans:
                # Keep what lies before and after this data section.
                pieces = [
                    piece
                    for lo, hi in pieces
                    for piece in ((lo, min(hi, data_lo)), (max(lo, data_hi), hi))
                    if piece[0] < piece[1]
                ]
            cached.extend(pieces)
        elf._file_backed_exec_ranges = cached
    return cached


def file_backed_exec_ranges(elf):
    """The file-backed executable PT_LOAD ranges of `elf`: the public entry other tools use.

    The list is computed once per Elf and cached on the elf object (see
    _file_backed_exec_ranges), so repeated calls return the same list without re-walking
    the segment table. Callers must not mutate it.
    """
    return _file_backed_exec_ranges(elf)


def has_import_table(elf):
    """The analyzer's gate for parsing imports: a module-info section, or a stub section
    with relocations. Shared with the import-stub census so the two never disagree."""
    return elf.sec(".rodata.sceModuleInfo") is not None or (
        elf.sec(".sceStub.text") is not None and elf.reloc is not None)


def _import_stub_is_file_executable(addr, file_exec_ranges):
    """True when an import stub's two words are both file-backed executable bytes."""
    return ((addr & 3) == 0 and in_ranges(addr, file_exec_ranges)
            and in_ranges(addr + 4, file_exec_ranges))


def _relocated_words(elf):
    """The words the PSP loader rewrites in a relocatable module, by field, or None.

    A relocatable module (ET_SCE_PRX with relocations) can load anywhere, so the loader
    writes every absolute address it holds: a code pointer in data carries a 32-bit
    relocation, a j/jal a 26-bit one and an `la` a HI16/LO16 pair. A word with no
    relocation is not an address at any load address. In such a module it is therefore
    no evidence of code: a small integer in .data that equals a .text offset of a
    zero-based module, or a j/jal word that a heuristic seed walked into, is data. None for
    every other image, whose absolute words are its addresses as they stand. The map is
    {word address: "word32" | "jump26" | "hi16" | "lo16"} (prxload.Prx.relocated).
    """
    return getattr(elf, "relocated_words", None)


def _relocated_jumps(elf):
    """The relocation map that decides j/jal evidence, or None when every jump counts.

    An unrelocated j/jal runs to its face value. A relocatable module placed at a load
    address (a nonzero base) whose own calls carry no relocation was linked for that
    address, so their face values are its addresses there. Without a load address (base
    0) such a face value cannot be told apart from an address in the executable that a
    module calls by link address, so only the relocated jumps are edges.
    """
    relocated = _relocated_words(elf)
    return None if relocated is None or getattr(elf, "base", None) else relocated


def _relocated_as(relocated, address, field):
    """True when `address` may serve as address evidence: the image is not relocatable, or
    the loader rewrites that word's `field` (see _relocated_words)."""
    return relocated is None or relocated.get(address) == field


#: The most words one called pointer array may hold (called_pointer_array_entries).
POINTER_ARRAY_MAX_ENTRIES = 0x10000
#: How many words of a candidate walker are read: the loop is a short leaf of its caller.
POINTER_ARRAY_WALKER_MAX_WORDS = 64
#: How many forwarding wrappers may stand between a call site and the walking loop.
POINTER_ARRAY_WALKER_MAX_DEPTH = 4
#: General registers a call may change: at, v0-v1, a0-a3, t0-t9 and ra.
_CALL_CLOBBERED_GPRS = frozenset((1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31))
#: Opcodes that write no general register: branches (beq..bgtz and the likely forms) and
#: stores (sb..sw, swl/swr, sc, swc1, the VFPU stores and sdc1).
_NO_GPR_WRITE_OPS = frozenset((
    0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17,
    0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x38, 0x39, 0x3A, 0x3D, 0x3E, 0x3F,
))


def _executable_segment_spans(elf):
    """The file-backed bytes of every executable PT_LOAD: what the PSP loader maps runnable."""
    return sorted(
        (segment["vaddr"], segment["vaddr"] + segment["filesz"])
        for segment in getattr(elf, "segments", ())
        if segment["type"] == 1 and segment.get("flags", 0) & 1 and segment["filesz"] > 0
    )


def _containing_span(address, spans):
    return next((span for span in spans if span[0] <= address < span[1]), None)


def _is_pointer_array_walker(elf, entry, memo=None, depth=0):
    """True when the function at `entry` calls every word of the array [a0, a1).

    Start-up code runs C++ static constructors this way: it passes the bounds of a table
    of function pointers to a short loop that loads each word and calls it
    (`for (p = start; p < end; p++) (*p)();`). The loop is read from its own instructions,
    in address order up to the first `jr $ra`: a cursor that starts as a0 and moves by 4
    is dereferenced, and the word loaded is the target of a `jalr $ra`; the cursor steps
    by 4; it is compared with the end, a1; and a branch after the call goes back to or
    before it. The cursor and the end may sit in registers or in the function's stack
    slots, where an unoptimised build keeps them. A function that passes its own a0 and
    a1 on unchanged to a walker (`jal`) walks the array too. An instruction the reading
    does not model makes what it writes unknown, so an unfamiliar shape is not a walker.
    `memo` caches answers by entry across calls.
    """
    if memo is None:
        memo = {}
    if entry in memo:
        return memo[entry]
    if depth > POINTER_ARRAY_WALKER_MAX_DEPTH:
        return False
    memo[entry] = False  # until answered: a call cycle is not a walker
    cursor, end, element = "cursor", "end", "element"
    regs = {4: cursor, 5: end}
    slots = {}
    called_at = None
    stepped = compared = looped = forwards = False
    stop_at = entry + 4 * POINTER_ARRAY_WALKER_MAX_WORDS

    def value(reg):
        return None if reg == 0 else regs.get(reg)

    def write(reg, val):
        if reg == 0:
            return
        if val is None:
            regs.pop(reg, None)
        else:
            regs[reg] = val

    pc = entry
    pending = None  # (address after a transfer's delay slot, direct target or None, is a jump)
    while True:
        if pending is not None and pc == pending[0]:
            target, jump = pending[1], pending[2]
            pending = None
            if (target is not None and value(4) == cursor and value(5) == end
                    and _is_pointer_array_walker(elf, target, memo, depth + 1)):
                forwards = True
            if jump:
                break
            for reg in _CALL_CLOBBERED_GPRS:
                regs.pop(reg, None)
        if pc >= stop_at:
            break
        raw = elf.read_at_vaddr(pc, 4)
        if raw is None or len(raw) < 4:
            return False
        word = int.from_bytes(raw, "little")
        op, rs, rt, rd = word >> 26, (word >> 21) & 0x1F, (word >> 16) & 0x1F, (word >> 11) & 0x1F
        simm = (word & 0xFFFF) - (0x10000 if word & 0x8000 else 0)
        if op == 0:
            funct = word & 0x3F
            if funct == 0x08:  # jr: only `jr $ra` ends the walker, after its delay slot
                if rs != 31:
                    return False
                stop_at = pc + 8
            elif funct == 0x09:  # jalr rd, rs: the call through the loaded word
                if rd == 31 and value(rs) == element:
                    called_at = pc
                write(rd, None)
                pending = (pc + 8, None, False)
            elif funct in (0x21, 0x25) and 0 in (rs, rt):  # addu/or with $zero: a move
                write(rd, value(rs or rt))
            elif funct in (0x2A, 0x2B):  # slt/sltu
                if {value(rs), value(rt)} == {cursor, end}:
                    compared = True
                write(rd, None)
            else:
                write(rd, None)
        elif op == 0x09:  # addiu rt, rs, simm
            if rs == 29 and rt == 29:
                slots = {}  # a new frame: earlier slot offsets name other words
            elif value(rs) == cursor and simm == 4:
                stepped = True
                write(rt, cursor)
            else:
                write(rt, value(rs) if simm == 0 else None)
        elif op == 0x23:  # lw rt, simm(rs)
            if rs == 29:
                write(rt, slots.get(simm))
            else:
                write(rt, element if value(rs) == cursor and simm == 0 else None)
        elif op == 0x2B:  # sw rt, simm(rs)
            if rs == 29:
                if value(rt) is None:
                    slots.pop(simm, None)
                else:
                    slots[simm] = value(rt)
        elif op in (0x02, 0x03):  # j / jal: a call, or a tail jump that ends the function
            target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            if op == 0x03:
                write(31, None)
            else:
                stop_at = pc + 8
            pending = (pc + 8, target, op == 0x02)
        elif op == 0x01 or op in _NO_GPR_WRITE_OPS:
            if op in (0x04, 0x05, 0x14, 0x15) and {value(rs), value(rt)} == {cursor, end}:
                compared = True
            if op == 0x01 and rt in REGIMM_LINK_RT:
                write(31, None)
            target = branch_target(pc, word)
            if called_at is not None and target is not None and target <= called_at < pc:
                looped = True
        elif op == 0x11 and rs in (0x00, 0x02):  # mfc1/cfc1 write rt
            write(rt, None)
        elif op != 0x11 and op not in (0x31, 0x35, 0x36):  # other GPR writers: rt or rd
            write(rt, None)
            write(rd, None)
        pc += 4
    memo[entry] = forwards or (called_at is not None and stepped and compared and looped)
    return memo[entry]


def _constant_arguments_at_calls(elf, covered, relocated):
    """Yield (target, a0, a1) for each traced direct `jal`/`j` with constant a0 and a1.

    A constant is materialised by `lui` and `addiu`/`ori` in the same straight-line run of
    traced words, the call's delay slot included. A run ends at a gap, after a control
    transfer's delay slot, and at any branch or jump target, where another path joins with
    other values. In a relocatable module each constant's low half must be a LO16 the
    loader relocates, and the call a jump that names the module (_relocated_jumps), as for
    every other address evidence.
    """
    addresses = sorted(covered)
    words = {}
    for address in addresses:
        raw = elf.read_at_vaddr(address, 4)
        if raw is not None and len(raw) == 4:
            words[address] = int.from_bytes(raw, "little")
    joins = set()
    for address, word in words.items():
        op = word >> 26
        if op == 2:
            joins.add((address & 0xF0000000) | ((word & 0x3FFFFFF) << 2))
        elif op != 3:
            target = branch_target(address, word)
            if target is not None:
                joins.add(target)
    consts = {}  # reg -> (value, address of the instruction that completed it)
    pending = None  # (transfer address, call target or None)
    previous = None
    for address in addresses:
        word = words.get(address)
        if word is None or previous is None or address != previous + 4 or address in joins:
            consts = {}
            if pending is not None and address != pending[0] + 4:
                pending = None
        previous = address
        if word is None:
            continue
        op, rs, rt, rd = word >> 26, (word >> 21) & 0x1F, (word >> 16) & 0x1F, (word >> 11) & 0x1F
        if op == 0x0F:  # lui
            consts[rt] = ((word & 0xFFFF) << 16, None)
        elif op in (0x09, 0x0D) and rs in consts:  # addiu/ori on a constant
            base = consts[rs][0]
            low = (word & 0xFFFF) - (0x10000 if op == 0x09 and word & 0x8000 else 0)
            consts[rt] = (((base + low) if op == 0x09 else (base | low)) & 0xFFFFFFFF, address)
        elif op == 0 and (word & 0x3F) in (0x21, 0x25) and 0 in (rs, rt) and (rs or rt) in consts:
            consts[rd] = consts[rs or rt]
        elif op == 0:
            consts.pop(rd, None)
        elif op not in _NO_GPR_WRITE_OPS and op not in (0x01, 0x02, 0x03, 0x11, 0x31, 0x35, 0x36):
            consts.pop(rt, None)
            if op in (0x1C, 0x1F):  # SPECIAL2/SPECIAL3 forms that write rd (seb, wsbh, ...)
                consts.pop(rd, None)
        consts.pop(0, None)
        if pending is not None and address == pending[0] + 4:
            transfer, target = pending
            pending = None
            a0, a1 = consts.get(4), consts.get(5)
            if (target is not None and a0 is not None and a1 is not None
                    and _relocated_as(_relocated_jumps(elf), transfer, "jump26")
                    and all(lo is not None and _relocated_as(relocated, lo, "lo16")
                            for _, lo in (a0, a1) if relocated is not None)):
                yield target, a0[0], a1[0]
            consts = {}
            continue
        if op in (0x02, 0x03):
            pending = (address, (address & 0xF0000000) | ((word & 0x3FFFFFF) << 2))
        elif (op == 0 and (word & 0x3F) in (0x08, 0x09)) or branch_target(address, word) is not None:
            pending = (address, None)


def called_pointer_array_entries(elf, covered, relocated=None):
    """Function entries in the pointer arrays that traced code passes to a walker.

    For every traced call or jump to a pointer-array walker (_is_pointer_array_walker)
    with a constant array [a0, a1), each nonzero word of the array is an entry: the
    walker calls it. An array whose words are not all aligned addresses in executable
    segment bytes (or, in a relocatable module, 32-bit relocations) is not understood
    and seeds nothing. The bytes need not be in a named code section: a section table
    that labels the constructors `.data` does not move them out of the executable
    segment the PSP runs them from.
    """
    executable = _executable_segment_spans(elf)
    walkers = {}
    entries = set()
    for target, start, end in _constant_arguments_at_calls(elf, covered, relocated):
        if (start >= end or (start | end) & 3
                or (end - start) // 4 > POINTER_ARRAY_MAX_ENTRIES):
            continue
        if not _is_pointer_array_walker(elf, target, walkers):
            continue
        blob = elf.read_at_vaddr(start, end - start)
        if blob is None or len(blob) != end - start:
            continue
        table = []
        for offset in range(0, len(blob), 4):
            word = int.from_bytes(blob[offset:offset + 4], "little")
            if word == 0:
                continue
            if ((word & 3) or not in_ranges(word, executable)
                    or not _relocated_as(relocated, start + offset, "word32")):
                table = None
                break
            table.append(word)
        if table:
            entries.update(table)
    return entries


def trace_function(elf, start, ranges, covered, calls, hc, walked=None, unrelocated=None):
    # Recursive descent over one function's intra-procedural control flow from `start`.
    # Adds every instruction address reached to `covered`, and every direct-call target (jal,
    # or a linking branch to a callable, #968) to `calls`. Returns newly found targets so
    # callers can extend their worklists without rescanning all prior calls. Calls return,
    # so execution continues after the delay slot; jr and j and an unconditional b end a
    # path (no fall-through). Conditional branches fork: follow the target and continue
    # past the delay slot.
    # The range membership tests below run once per traced instruction, so they use an
    # index built once per trace (same answers as in_ranges over the plain list).
    # `walked` is the set of instruction addresses that earlier traces over these same
    # ranges already expanded (analyze owns it). Expanding an address depends only on its
    # encoding and the ranges; the start exception and the `calls` stop test are the only
    # trace-dependent parts, and both can only stop an expansion that an earlier trace
    # already did or stopped. An address in `walked` therefore ends this path: the
    # covered addresses, call targets and successors it would add are already present.
    # Without the memo every start re-expands the whole shared tail it reaches: a profiled
    # run of one large executable expanded each word about 190 times. `walked=None` is the
    # uncached walk, kept so tests can pin the memoized walk against it.
    # In a relocatable module analysed without a load address (_relocated_jumps) a j/jal the
    # loader does not relocate names an address outside the module (a module linked against
    # its executable calls into it this way), so its target is no edge into this module's
    # code; the word is added to `unrelocated` when the caller passes a set.
    relocated = _relocated_jumps(elf)
    ranges = span_index(ranges)
    stack = [start]
    local = set()
    new_calls = []
    while stack:
        pc = stack.pop()
        # Stop the linear scan when it reaches a DIFFERENT high-confidence function entry
        # (a jal target / address-taken code pointer / export) -- but only when that entry
        # is a genuine function boundary. hc is built from jal targets, la-materialized
        # code pointers, and exports (internal block labels live in `noisy`), so it can
        # also contain internal switch-case/jump-table landing pads that happen to be
        # address-taken without ever being a *direct* call target. Those pads never carry
        # an o32 prologue (`addiu sp, sp, -X`) of their own; treating them as hard barriers
        # truncates the owning function's switch body. Signature-aware check: a foreign hc
        # hit only truncates when it looks like a real entry (has the sp-alloc prologue) or
        # is a known direct-call target (`calls`, populated by jal in this same pass and
        # prior functions). Anything else falls through and is traced as this function's
        # own code, exactly like the case that recovers f_0000d518 -> f_0000d530 (that
        # split target has neither trait working against it: it IS a real jal target).
        # Without this guard entirely, a function that ends by falling through into the
        # next function -- e.g. one whose last act is `jal <noreturn>` with no epilogue of
        # its own -- swallows that adjacent function and adopts ITS `jr $ra` as its own
        # terminator, dropping its real return path. The mis-split then makes every caller
        # reload s0-s7/ra from off-by-frame stack slots (callee-save corruption; see
        # docs/audit/F3E_CALLEESAVE.md, the f_0005a648 bug class). The recursive jr-ra-fold
        # below (for hoisted trailing epilogues) still runs for pc+8 addresses that are not
        # treated as boundaries here.
        while in_ranges(pc, ranges) and pc not in local:
            if walked is not None and pc in walked:
                break
            if pc != start and pc in hc:
                insn_bytes = elf.read_at_vaddr(pc, 4)
                stop = True
                if insn_bytes and len(insn_bytes) == 4:
                    insn = int.from_bytes(insn_bytes, 'little')
                    is_prologue = (insn >> 16) == 0x27BD and (insn & 0x8000)
                    if not is_prologue and pc not in calls:
                        # Internal jump-table/switch landing pad, not a real
                        # function entry: keep tracing it as part of this function.
                        stop = False
                if stop:
                    break
            local.add(pc)
            if walked is not None:
                walked.add(pc)
            covered.add(pc)
            wb = elf.read_at_vaddr(pc, 4)
            if wb is None or len(wb) < 4:
                break
            word = int.from_bytes(wb, 'little')
            op = word >> 26
            funct = word & 0x3F
            if op in (2, 3) and not _relocated_as(relocated, pc, "jump26"):
                # Its target is not this module's code: a call (jal) continues past the delay
                # slot like any call, a jump (j) ends the path like a tail call.
                if unrelocated is not None:
                    unrelocated.add(pc)
                covered.add(pc + 4)
                if op == 2:
                    break
                pc += 8
                continue
            if op == 3:  # jal: direct call, returns -> continue past delay slot
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges) or in_ranges(target, _file_backed_exec_ranges(elf)):
                    if target not in calls:
                        calls.add(target)
                        new_calls.append(target)
                covered.add(pc + 4)
                pc += 8
                continue
            if (op == 1 and ((word >> 16) & 0x1F) in REGIMM_LINK_RT
                    and _linked_branch_is_call(elf, pc, word, ranges)):
                # bal/bgezal/bltzal (and the likely forms) to a callable: a call wearing
                # a branch encoding (#968). Its target is a function entry like a jal
                # target, never this function's interior, and both paths continue past
                # the delay slot. `bal .+8` and a frame-releasing continuation target
                # are not calls and fork below, exactly as before.
                target = branch_target(pc, word)
                if target not in calls and (
                    in_ranges(target, ranges) or in_ranges(target, _file_backed_exec_ranges(elf))
                ):
                    calls.add(target)
                    new_calls.append(target)
                covered.add(pc + 4)
                pc += 8
                continue
            if op == 2:  # j: tail call or local jump
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                covered.add(pc + 4)
                if in_ranges(target, ranges):
                    stub = elf.sec(".sceStub.text")
                    is_stub_target = stub and stub["addr"] <= target < stub["addr"] + stub["size"]
                    tb = elf.read_at_vaddr(target, 4)
                    is_prologue = False
                    if tb and len(tb) == 4:
                        tw = int.from_bytes(tb, 'little')
                        if (tw >> 16) == 0x27BD and (tw & 0x8000):
                            is_prologue = True
                    if target in hc or is_stub_target or is_prologue:
                        if target not in calls:
                            calls.add(target)
                            new_calls.append(target)
                        break
                    else:
                        stack.append(target)
                        break
            if op == 0 and funct == 0x08:  # jr (return / computed): end this path
                covered.add(pc + 4)
                # If the instruction after the delay slot is also jr $ra, the
                # function has multiple return paths (e.g. a tiny comparator
                # stub merged into the same address range). Continue tracing
                # so those instructions are covered by this function rather
                # than detected as a standalone thin stub.
                nx = elf.read_at_vaddr(pc + 8, 4)
                if nx and len(nx) == 4 and int.from_bytes(nx, 'little') == 0x03e00008:
                    pc += 8
                    continue
                break
            # Branches: REGIMM (1), beq/bne/blez/bgtz (4-7) and their likely forms (20-23),
            # and FPU bc1 (cop1 with rs=8). Target is intra-function; fork and continue.
            is_branch = op in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (op == 0x11 and ((word >> 21) & 0x1F) == 8)
            if is_branch:
                off = word & 0xFFFF
                off = off - 0x10000 if off & 0x8000 else off
                target = pc + 4 + (off << 2)
                if in_ranges(target, ranges):
                    stack.append(target)
                covered.add(pc + 4)
                # Unconditional b (beq $zero,$zero): no fall-through after the delay slot.
                if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
                    break
                pc += 8
                continue
            pc += 4
    return new_calls


def _is_hard_terminator(word):
    # True for instructions after which control never falls through to the next
    # slot-pair: `j`, any `jr`, unlinked `jalr $zero, $rs`, or the unconditional `b` idiom (beq $0,$0).
    # `jal`/linked `jalr`/`bal` return, so they are NOT terminators here.
    op = word >> 26
    if op == 2:
        return True
    if op == 0:
        fn = word & 0x3F
        if fn == 0x08:  # jr rs
            return True
        if fn == 0x09 and ((word >> 11) & 0x1F) == 0:  # jalr $zero, rs
            return True
        return False
    if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
        return True
    return False


#: Public alias. The entry-role audit needs exactly this fall-through rule to
#: decide whether an address can be entered by falling into it, and must not
#: carry a second, drifting copy of it.
is_hard_terminator = _is_hard_terminator


def _is_address_taken_entry(elf, x):
    # An address materialized in code (`lui` + `addiu`/`ori`) is a code pointer: a
    # callback, a thread entry, or a computed-goto base. It is a function entry when
    # its first word is a leaf return (`jr $ra`) or a stack-allocating prologue; a
    # computed-goto base is neither (bounded_computed_goto_targets resolves those),
    # so it stays a weak signal. A `jr $ra` sitting 8 bytes after another `jr $ra`
    # (the previous function's return and its delay slot) is unreachable by fall-
    # through, so taking its address is the only way in: it is a separate leaf
    # function (`return 0`, `return a0`), not a shared epilogue. Folding it into the
    # previous function left callbacks and thread entries with no dispatch entry.
    raw = elf.read_at_vaddr(x, 4)
    if raw is None or len(raw) < 4:
        return False
    word = int.from_bytes(raw, 'little')
    return word == 0x03e00008 or ((word >> 16) == 0x27BD and bool(word & 0x8000))


def direct_j_edges(elf, ranges, targets=None):
    """Return unconditional direct-``j`` edges in executable ranges.

    The analyzer already treats ``j`` as a weak boundary signal because the
    instruction is used for both tail transfers and intra-procedural control
    flow.  Keep the edge census separate from that boundary decision: callers
    need the actual source PCs to distinguish a one-way shared-tail transfer
    from a backwards loop edge.  ``targets`` optionally limits the result to a
    known entry set without changing the scan itself.

    The returned mapping is ``target -> tuple(sorted(source_pcs))``.  Only
    in-range targets are reported; data words outside executable ranges are
    never interpreted as edges.
    """
    wanted = None if targets is None else set(targets)
    edges = {}
    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            # Small synthetic images used by analyzer/codegen regressions often
            # expose only the scalar read surface.  Keep the production path
            # bulk-read based, but make the evidence helper honor that same
            # read contract without requiring an ELF container.
            words = (
                (source, elf.read_at_vaddr(source, 4))
                for source in range(lo, hi, 4)
            )
        else:
            words = (
                (lo + off, blob[off:off + 4])
                for off in range(0, len(blob) - 3, 4)
            )
        for source, raw in words:
            if raw is None or len(raw) < 4:
                continue
            word = int.from_bytes(raw, "little")
            if word >> 26 != 2:  # unconditional direct jump
                continue
            target = (source & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if not in_ranges(target, ranges):
                continue
            if wanted is not None and target not in wanted:
                continue
            edges.setdefault(target, []).append(source)
    return {target: tuple(sorted(sources)) for target, sources in edges.items()}


#: The subset of :data:`REGIMM_BRANCH_RT` that writes ``$ra``.  ``bal`` is
#: ``bgezal $zero``; these are *calls* wearing a branch encoding, so their
#: target is a callable boundary and never continuation evidence.
REGIMM_LINK_RT = frozenset({0x10, 0x11, 0x12, 0x13})


def linked_branch_target(source, word):
    """Target of a linking REGIMM branch that can be a call, or ``None``.

    ``None`` for every word that is not ``bltzal``/``bgezal``/``bltzall``/
    ``bgezall`` (``bal`` is ``bgezal $zero``), and for the read-the-PC idiom
    ``bal .+8``: a target equal to the link is the caller's own next pair, so
    that branch only materializes the PC and is never a call (#968).  Whether a
    returned target *is* a call also depends on the target's frame role; see
    :func:`entry_frame_balance.linked_branch_is_call`, which every walker uses.
    """
    if word >> 26 != 1 or ((word >> 16) & 0x1F) not in REGIMM_LINK_RT:
        return None
    target = branch_target(source, word)
    if target == (source + 8) & 0xFFFFFFFF:
        return None
    return target


def _linked_branch_is_call(elf, source, word, ranges):
    # entry_frame_balance imports this module, so it is imported where it is used.
    import entry_frame_balance
    return entry_frame_balance.linked_branch_is_call(elf, source, word, ranges)


#: Branch edge kinds reported by :func:`direct_branch_edges`.
BRANCH_COND = "cond"          # an ordinary two-way conditional branch
BRANCH_UNCOND = "uncond"      # the ``b`` idiom (``beq $zero, $zero``): no fall-through
BRANCH_LINK = "link"          # ``bal``/``bgezal``/...: a call, not a branch edge


def _iter_code_words(elf, ranges):
    """Yield ``(vaddr, word)`` over executable ranges.

    Bulk-read first, falling back to the scalar ``read_at_vaddr`` contract that
    the owned synthetic regressions expose, so the same census runs host-neutral
    without an ELF container.
    """
    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            for addr in range(lo, hi, 4):
                raw = elf.read_at_vaddr(addr, 4)
                if raw is not None and len(raw) >= 4:
                    yield addr, int.from_bytes(raw[:4], "little")
        else:
            for off in range(0, len(blob) - 3, 4):
                yield lo + off, int.from_bytes(blob[off:off + 4], "little")


def branch_target(source, word):
    """PC-relative branch target of ``word`` at ``source``, or ``None``.

    ``None`` means the word is not a direct branch at all.  The returned kind is
    supplied separately by :func:`direct_branch_edges`; this helper exists so the
    decode lives in exactly one place.
    """
    op = word >> 26
    if op == 1:
        if ((word >> 16) & 0x1F) not in REGIMM_BRANCH_RT:
            return None
    elif op in (4, 5, 20, 21):
        pass
    elif op in (6, 7, 22, 23):
        if ((word >> 16) & 0x1F) != 0:
            return None
    elif op in (0x11, 0x12) and ((word >> 21) & 0x1F) == 8:
        pass
    else:
        return None
    offset = word & 0xFFFF
    offset -= 0x10000 if offset & 0x8000 else 0
    return (source + 4 + (offset << 2)) & 0xFFFFFFFF


def branch_kind(word):
    """Classify a direct branch as conditional, unconditional, or linking."""
    op = word >> 26
    if op == 1 and ((word >> 16) & 0x1F) in REGIMM_LINK_RT:
        return BRANCH_LINK
    if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
        return BRANCH_UNCOND
    return BRANCH_COND


def direct_branch_edges(elf, ranges, targets=None):
    """Return direct PC-relative branch edges in executable ranges.

    The companion of :func:`direct_j_edges` for the conditional-branch slice of
    issue #51.  A conditional branch differs from ``j`` in ways the caller must
    see rather than infer, so the mapping is ``target -> ((source, kind), ...)``
    with the kind preserved:

    * an ordinary conditional branch leaves a **fall-through** predecessor
      behind it, so its target is not entered by that edge alone;
    * the ``b`` idiom has no fall-through, exactly like ``j``;
    * a **linking** REGIMM branch is a call and makes its target a callable
      boundary -- keeping it in the census, rather than dropping it, is what
      lets the audit veto instead of silently missing the evidence.

    ``targets`` optionally limits the result to a known entry set.  Only in-range
    targets are reported.
    """
    wanted = None if targets is None else set(targets)
    edges = {}
    for source, word in _iter_code_words(elf, ranges):
        target = branch_target(source, word)
        if target is None or not in_ranges(target, ranges):
            continue
        if wanted is not None and target not in wanted:
            continue
        edges.setdefault(target, []).append((source, branch_kind(word)))
    return {target: tuple(sorted(srcs)) for target, srcs in edges.items()}


#: How far back from a computed ``jr`` the bounded-goto match looks, and the
#: most landing pads one goto may name. A wider mask is left unresolved.
COMPUTED_GOTO_WINDOW = 16
COMPUTED_GOTO_MAX_TARGETS = 64


def _gpr_writes(word):
    """GPRs ``word`` writes: a frozenset, ``None`` for a control transfer, or
    ``"unknown"`` for an encoding this decoder does not classify."""
    op = word >> 26
    rs, rt, rd = (word >> 21) & 0x1F, (word >> 16) & 0x1F, (word >> 11) & 0x1F
    if op in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23):
        return None
    if op == 0:
        funct = word & 0x3F
        if funct in (0x08, 0x09):
            return None
        if funct in (0x0C, 0x0D, 0x0F, 0x11, 0x13) or 0x18 <= funct <= 0x1F:
            return frozenset()  # syscall/break/sync, mthi/mtlo, mult/div/madd/msub
        return frozenset({rd})
    if 0x08 <= op <= 0x0F or 0x20 <= op <= 0x26 or op in (0x30, 0x38):
        return frozenset({rt})  # ALU immediates, loads, ll/sc
    if 0x28 <= op <= 0x2F or op in (0x31, 0x32, 0x35, 0x36, 0x39, 0x3A, 0x3D, 0x3E):
        return frozenset()  # stores, cache, FPU/VFPU loads and stores
    if op == 0x11:
        if rs == 8:
            return None  # bc1
        return frozenset({rt}) if rs in (0, 2) else frozenset()  # mfc1/cfc1
    if op == 0x1F:
        funct = word & 0x3F
        if funct in (0x00, 0x04):
            return frozenset({rt})  # ext/ins
        if funct == 0x20:
            return frozenset({rd})  # seb/seh/wsbh/bitrev
    return "unknown"


def _defining_word(elf, start, use_addr, reg):
    """The last word in ``[start, use_addr)`` that writes ``reg``, as
    ``(addr, word)``, or ``None`` if none does or the straight-line run is
    broken by a control transfer or an unclassified encoding."""
    for addr in range(use_addr - 4, start - 4, -4):
        raw = elf.read_at_vaddr(addr, 4)
        if raw is None or len(raw) < 4:
            return None
        word = int.from_bytes(raw, "little")
        writes = _gpr_writes(word)
        if writes is None or writes == "unknown":
            return None
        if reg in writes:
            return addr, word
    return None


def bounded_computed_goto_targets(elf, ranges):
    """Landing pads of computed ``jr`` whose target set the code bounds itself.

    The compiler's unrolled store/copy ladders enter at ``base -/+ (x & mask)``::

        andi  idx, n, mask       # optionally followed by sll idx, idx, k
        lui   at, %hi(base)
        addiu at, at, %lo(base)  # or ori
        subu  at, at, idx        # or addu at, at, idx / addu at, idx, at
        jr    at

    Every target is ``base`` minus (or plus) one value of ``x & mask``, so the
    whole set is known statically. Each pad must be word-aligned and inside an
    analyzer-owned range; a pad outside one, a mask naming more than
    :data:`COMPUTED_GOTO_MAX_TARGETS` pads, or any other write to the index or
    base register between its definition and the ``jr`` leaves the goto
    unresolved (it still dispatches fail-closed at runtime). All matched words
    must be one straight-line run with no control transfer between them.
    """
    targets = set()
    for addr, word in _iter_code_words(elf, ranges):
        if word >> 26 != 0 or (word & 0x3F) != 0x08:
            continue
        reg = (word >> 21) & 0x1F
        if reg in (0, 31):
            continue
        window = max(addr - 4 * COMPUTED_GOTO_WINDOW, 0)
        combine = _defining_word(elf, window, addr, reg)
        if combine is None:
            continue
        c_addr, c_word = combine
        funct = c_word & 0x3F
        if c_word >> 26 != 0 or funct not in (0x21, 0x23):
            continue
        rs, rt = (c_word >> 21) & 0x1F, (c_word >> 16) & 0x1F
        operands = [(rs, rt)] if funct == 0x23 else [(rs, rt), (rt, rs)]
        for base_reg, idx_reg in operands:
            base = _materialized_address(elf, window, c_addr, base_reg)
            mask = _masked_index(elf, window, c_addr, idx_reg)
            if base is None or mask is None:
                continue
            values = []
            sub = mask
            while True:
                if sub % 4 == 0:
                    values.append(sub)
                if sub == 0:
                    break
                sub = (sub - 1) & mask
            if len(values) > COMPUTED_GOTO_MAX_TARGETS:
                break
            sign = -1 if funct == 0x23 else 1
            pads = {(base + sign * value) & 0xFFFFFFFF for value in values}
            if all(in_ranges(pad, ranges) for pad in pads):
                targets.update(pads)
            break
    return frozenset(targets)


def _materialized_address(elf, window, use_addr, reg):
    # `lui reg', hi` followed by `addiu/ori reg, reg', lo`, the `la` idiom.
    low = _defining_word(elf, window, use_addr, reg)
    if low is None:
        return None
    l_addr, l_word = low
    op = l_word >> 26
    if op not in (0x09, 0x0D):
        return None
    src = (l_word >> 21) & 0x1F
    high = _defining_word(elf, window, l_addr, src)
    if high is None or high[1] >> 26 != 0x0F:
        return None
    upper = (high[1] & 0xFFFF) << 16
    if op == 0x09:
        lo = l_word & 0xFFFF
        return (upper + lo - (0x10000 if lo & 0x8000 else 0)) & 0xFFFFFFFF
    return upper | (l_word & 0xFFFF)


def _masked_index(elf, window, use_addr, reg):
    # `andi reg, src, mask`, optionally shifted by `sll reg, reg, k`.
    found = _defining_word(elf, window, use_addr, reg)
    if found is None:
        return None
    f_addr, f_word = found
    shift = 0
    if f_word >> 26 == 0 and (f_word & 0x3F) == 0x00 and ((f_word >> 16) & 0x1F) == reg:
        shift = (f_word >> 6) & 0x1F
        found = _defining_word(elf, window, f_addr, reg)
        if found is None:
            return None
        f_word = found[1]
    if f_word >> 26 != 0x0C:
        return None
    return ((f_word & 0xFFFF) << shift) & 0xFFFFFFFF


def code_pointer_evidence(elf, ranges):
    """Census of every way an in-range address is reachable other than by falling into it.

    Returns ``{kind: frozenset(addresses)}`` over four independent kinds:

    ``jal``
        a direct call target.
    ``branch-link``
        a ``bal``/``bgezal``-family target -- also a call.
    ``immediate``
        an address materialized as a constant in code (``lui`` + ``addiu``/``ori``),
        i.e. taken for an indirect call, a callback, or a stored pointer.
    ``data``
        an aligned word found in an allocated non-code section that points
        into an analyzer-owned range or a file-backed executable segment.

    :func:`analyze` folds these signals into its start set and then discards
    *which* signal fired.  The entry-role audit needs them kept apart, because
    each one contradicts a continuation classification for a different reason:
    the first two prove a fresh-call contract outright, while the last two prove
    only that the address is independently dispatchable with an incoming
    contract this analysis cannot see.
    """
    evidence = {kind: set() for kind in ("jal", "branch-link", "immediate", "data")}
    file_exec_ranges = _file_backed_exec_ranges(elf)
    # A relocatable module's addresses are only the words its loader relocates (its jumps:
    # see _relocated_jumps).
    relocated = _relocated_words(elf)
    jumps = _relocated_jumps(elf)

    hireg = {}
    clear_next = False
    for addr, word in _iter_code_words(elf, ranges):
        if clear_next:
            hireg.clear()
            clear_next = False
        op = word >> 26
        if op == 3:  # jal
            target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            if in_ranges(target, ranges) and _relocated_as(jumps, addr, "jump26"):
                evidence["jal"].add(target)
        elif op == 1 and ((word >> 16) & 0x1F) in REGIMM_LINK_RT:
            target = branch_target(addr, word)
            if target is not None and in_ranges(target, ranges):
                evidence["branch-link"].add(target)

        if op == 0x0F:  # lui rt, imm
            hireg[(word >> 16) & 0x1F] = (word & 0xFFFF) << 16
        elif op in (0x09, 0x0D):  # addiu / ori: the low half of an `la`
            base = hireg.get((word >> 21) & 0x1F)
            if isinstance(base, int):
                if op == 0x09:
                    low = (word & 0xFFFF) - (0x10000 if word & 0x8000 else 0)
                    value = (base + low) & 0xFFFFFFFF
                else:
                    value = base | (word & 0xFFFF)
                if (in_ranges(value, ranges) and (value & 3) == 0
                        and _relocated_as(relocated, addr, "lo16")):
                    evidence["immediate"].add(value)
        if branch_target(addr, word) is not None or op in (2, 3) or (
            op == 0 and (word & 0x3F) in (0x08, 0x09)
        ):
            clear_next = True

    # Non-code sections. `sections` is absent on the owned synthetic word images
    # used by the regressions, which carry no data segment at all; treat that as
    # "no data evidence" rather than requiring the fixtures to fake a container.
    for section in getattr(elf, "sections", ()):
        if (
            section.get("typ") != 1
            or not section.get("size")
            or not (section.get("flags", 0) & 2)
        ):
            continue
        if section.get("nm") in (".text", ".sceStub.text"):
            continue
        blob = section_bytes(elf, section)
        for off in range(0, len(blob) - 3, 4):
            value = int.from_bytes(blob[off:off + 4], "little")
            if (
                (in_ranges(value, ranges) or in_ranges(value, file_exec_ranges))
                and (value & 3) == 0
                and _relocated_as(relocated, section["addr"] + off, "word32")
            ):
                evidence["data"].add(value)

    return {kind: frozenset(values) for kind, values in evidence.items()}


class _Discovery:
    """The state analyze() builds up: the executable ranges, the seeds and the traced functions.

    The passes below share it by reference and mutate its sets in place, in the order analyze()
    runs them. Each pass keeps the body it had inside analyze(); it only opens by naming the state
    it uses.
    """

    def __init__(self, elf, extra_spans, cfg_gate, trace_memo):
        self.elf = elf
        self.cfg_gate = cfg_gate
        self.ranges = exec_ranges(elf, extra_spans=extra_spans)
        self.file_exec_ranges = _file_backed_exec_ranges(elf)
        self.relocated = _relocated_words(elf)
        self.jumps = _relocated_jumps(elf)
        # Executable segment bytes that a called pointer array proves hold code although the
        # section table names them data (see called_pointer_array_entries). Filled after the
        # first discovery round; only control flow from those entries reaches into them.
        self.proven_spans = []
        # High-confidence function starts: addresses that are genuinely entered as a function,
        # not internal blocks. These seed the extent tracing below.
        self.hc = set()
        self.noisy = set()
        self.jtails = set()  # every in-range `j` target; candidates for tail-call promotion below
        # `covered` ends up holding every instruction that belongs to some known function, so the
        # weak signals that land inside a function body (internal blocks) can be discarded.
        self.covered = set()
        self.calls = set()
        self.functions = set()
        # Words expanded by earlier traces, one set per range list (see trace_function's
        # `walked`). Traces share a set only when their ranges are equal, so a word expanded
        # under one range list is never skipped under another.
        self.walked_by_ranges = {} if trace_memo else None

    def trace_ranges_for_entry(self, entry):
        ranges = self.ranges
        if in_ranges(entry, ranges):
            return ranges
        containing = next(
            (span for span in (*self.file_exec_ranges, *self.proven_spans) if span[0] <= entry < span[1]),
            None,
        )
        if containing is None:
            return ranges
        spans = sorted([*ranges, containing])
        merged = []
        for lo, hi in spans:
            if merged and lo <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
            else:
                merged.append((lo, hi))
        return merged

    def in_text(self, a):
        return in_ranges(a, self.ranges) and (a & 3) == 0

    def trace(self, entry, entry_ranges):
        walked = None
        if self.walked_by_ranges is not None:
            walked = self.walked_by_ranges.setdefault(tuple(entry_ranges), set())
        return trace_function(
            self.elf, entry, entry_ranges, self.covered, self.calls, self.hc, walked=walked,
        )

    def callable_target(self, addr):
        # The worklist's rule for a direct-call or jump target, shared by the late passes:
        # named code, or file-backed executable bytes outside the named sections. The late
        # passes tested the named ranges alone, so a callee in those bytes that only a late
        # pass reached was dropped and got no body (the follow-up to the import-stub seed).
        # The two tests are deliberately asymmetric. `ranges` already carries the title's
        # extra executable spans (exec_ranges), which need not be file-backed, while
        # `file_exec_ranges` is only the file-backed PT_LOAD executable set. So a target in
        # an extra span that is not file-backed is accepted through `ranges` alone. A
        # proven span (a called pointer array's targets) is callable the same way; only a
        # trace that starts inside it can name a target there.
        return (in_ranges(addr, self.ranges) or in_ranges(addr, self.file_exec_ranges)
                or in_ranges(addr, self.proven_spans))

    def discover(self, work):
        # Trace from `work`, following discovered calls, then seed the pointer arrays the
        # traced code calls through, until neither finds anything new.
        elf, covered, relocated = self.elf, self.covered, self.relocated
        functions, hc, proven_spans = self.functions, self.hc, self.proven_spans
        callable_target, trace = self.callable_target, self.trace
        trace_ranges_for_entry = self.trace_ranges_for_entry
        while True:
            while work:
                s = work.pop()
                new_calls = []
                if callable_target(s):
                    new_calls = trace(s, trace_ranges_for_entry(s))
                for t in new_calls:
                    if t not in functions and callable_target(t):
                        functions.add(t)
                        work.append(t)
            # Start-up code calls every word of a function-pointer array (C++ static
            # constructors). Each word is a function entry even where the section table
            # names the bytes data: some retail section tables label everything after the
            # import tables `.data`, constructors included. The traced call sites are the
            # evidence, so this runs once the traced code is known.
            seeds = set()
            for entry in called_pointer_array_entries(elf, covered, relocated):
                if entry in functions:
                    continue
                if not callable_target(entry):
                    span = _containing_span(entry, _executable_segment_spans(elf))
                    if span is None:
                        continue
                    if span not in proven_spans:
                        proven_spans.append(span)
                seeds.add(entry)
            if not seeds:
                return
            hc.update(seeds)
            functions.update(seeds)
            work.extend(sorted(seeds))


def _seed_entry_point(d):
    elf, ranges, file_exec_ranges, hc = d.elf, d.ranges, d.file_exec_ranges, d.hc
    if (elf.entry & 3) == 0 and (
        in_ranges(elf.entry, ranges) or in_ranges(elf.entry, file_exec_ranges)
    ):
        hc.add(elf.entry)
    # Some module-start trampolines begin at an executable PT_LOAD's first byte,
    # before the named .text section, and are reached through the loader rather
    # than a statically visible pointer. Seed only the segment boundary when it
    # has the usual negative stack-allocation prologue; CFG tracing below still
    # limits ownership to instructions reachable from that candidate.
    for start, _ in file_exec_ranges:
        if (start & 3) or in_ranges(start, ranges):
            continue
        first = elf.read_at_vaddr(start, 4)
        if first is None or len(first) < 4:
            continue
        word = int.from_bytes(first, "little")
        if (word >> 16) == 0x27BD and (word & 0x8000):
            hc.add(start)


def _seed_prologues_and_terminators(d):
    elf, ranges, noisy = d.elf, d.ranges, d.noisy
    # Build a file-offset -> vaddr lookup from PT_LOAD segments (needed because
    # the raw file's byte offset differs from the guest virtual address for PRX/rebased ELFs).
    # If no PT_LOAD segment covers a given file offset, the address is skipped.
    loads = [s for s in elf.segments if s["type"] == 1]
    def file_to_vaddr(fo):
        for s in loads:
            if s["off"] <= fo < s["off"] + s["filesz"]:
                return s["vaddr"] + (fo - s["off"])
        return None

    # NEW: Add all potential prologues and post-terminator addresses as seeds
    data = elf.data
    for i in range(0, len(data) - 4, 4):
        w = int.from_bytes(data[i:i+4], 'little')
        va = file_to_vaddr(i)
        if va is None:
            continue
        # addiu sp, sp, -X (0x27bdXXXX where XXXX is negative)
        if (w >> 16) == 0x27BD and (w & 0x8000) and (va & 3) == 0:
            noisy.add(va)
        # jr $ra / unconditional b terminators. The instruction after the delay slot
        # is very likely the start of a new function.
        # We explicitly omit 'j' (op == 2) because PSP pointers start with 0x08, making
        # every pointer look like a 'j' instruction, which pollutes the function list
        # with data/string addresses.
        is_uncond = (w == 0x03e00008) or ((w >> 16) == 0x1000)
        if is_uncond and in_ranges(va + 8, ranges) and ((va + 8) & 3) == 0:
            noisy.add(va + 8)


def _seed_module_exports(d):
    elf, ranges, relocated, hc = d.elf, d.ranges, d.relocated, d.hc
    # Module export pointers and constructor/destructor arrays point at real functions.
    # NOTE: .lib.ent is parsed separately below (structures with func_table), don't scan raw.
    for nm in (".rodata.sceModuleInfo", ".rodata.sceResident",
               ".ctors", ".dtors", ".init_array", ".fini_array"):
        s = elf.sec(nm)
        if not s or s["typ"] == 8:
            continue
        blob = section_bytes(elf, s)
        for o in range(0, len(blob) - 3, 4):
            val = int.from_bytes(blob[o:o + 4], 'little')
            if (in_ranges(val, ranges) and (val & 3) == 0
                    and _relocated_as(relocated, s["addr"] + o, "word32")):
                hc.add(val)
    # Exported functions from .lib.ent. Each export record is a 16-byte head --
    # library name, version, attribute, length in words (at least 4), variable
    # count, function count, entry table -- and the entry table holds the
    # (functions + variables) NIDs followed by as many addresses, functions
    # first. Every exported function address (module_start/module_stop and each
    # library function another module links to) is a real function start, even
    # when no call or prologue heuristic would find it. Variable exports are
    # data, never seeds.
    libent = elf.sec(".lib.ent")
    if libent:
        blob = section_bytes(elf, libent)
        idx = 0
        while idx + 16 <= len(blob):
            _name_ptr, _ver, _flags, size, num_vars, num_funcs, entry_table = struct.unpack(
                "<IHHBBHI", blob[idx:idx + 16])
            if size < 4:
                break  # malformed: a record shorter than its own head ends the table
            count = num_funcs + num_vars
            if num_funcs and entry_table:
                addresses = elf.read_at_vaddr(entry_table + 4 * count, 4 * num_funcs)
                if addresses is not None and len(addresses) == 4 * num_funcs:
                    for f_idx in range(num_funcs):
                        fptr = int.from_bytes(addresses[4 * f_idx:4 * f_idx + 4], 'little')
                        if in_ranges(fptr, ranges) and (fptr & 3) == 0:
                            hc.add(fptr)
            idx += size * 4


def _seed_import_stubs(d):
    elf, ranges, file_exec_ranges, hc = d.elf, d.ranges, d.file_exec_ranges, d.hc
    # Reconstruct stubs from .sceStub.text or imports
    stubs_sec = elf.sec(".sceStub.text")
    if has_import_table(elf):
        try:
            from imports import ImportTableError, parse_imports
            impmap = parse_imports(elf)
            for a in impmap.keys():
                if in_ranges(a, ranges) and (a & 3) == 0:
                    hc.add(a)
                elif _import_stub_is_file_executable(a, file_exec_ranges):
                    # A retail import stub lives in executable PT_LOAD bytes outside the
                    # named code sections (.text, .sceStub.text), so `ranges` never holds
                    # it. The late discovery passes (tail promotion and gap fill) keep a
                    # callee only when it is inside `ranges`, so a stub that was reached
                    # only through a direct call from an orphan function was dropped
                    # there. Codegen then emitted no body, and the call dispatched to the
                    # image's `jr $ra; nop` placeholder, which returns a stale $v0 with no
                    # HLE event. Seeding the stub as an entry here makes it a traced
                    # function that owns its two words before the gap-fill safety nets
                    # run. Stubs that are not file-backed executable bytes are not seeded.
                    hc.add(a)
        except Exception as e:
            from imports import ImportTableError
            if isinstance(e, ImportTableError):
                raise
            raise ImportTableError(
                "ANALYZER_IMPORT_TABLE_INVALID",
                f"failed to parse imports in analyzer: {e}",
            ) from e
    elif stubs_sec:
        for a in range(stubs_sec["addr"], stubs_sec["addr"] + stubs_sec["size"], 8):
            if in_ranges(a, ranges) and (a & 3) == 0:
                hc.add(a)


def _seed_function_pointer_tables(d, entry_hints):
    elf, ranges, relocated, hc, in_text = d.elf, d.ranges, d.relocated, d.hc, d.in_text
    # Function-pointer tables in read-only/data sections (callbacks reached via jalr).
    for nm in (".rodata", ".data", ".sdata", ".rodata.sceResident"):
        s = elf.sec(nm)
        if not s or s["typ"] == 8:
            continue
        blob = section_bytes(elf, s)
        for o in range(0, len(blob) - 3, 4):
            val = int.from_bytes(blob[o:o + 4], 'little')
            if (in_ranges(val, ranges) and (val & 3) == 0
                    and _relocated_as(relocated, s["addr"] + o, "word32")):
                hc.add(val)

    # Linker-specific data-section names can also contain function pointers.
    # Keep their file-backed executable targets as traced entry candidates.
    hc.update(code_pointer_evidence(elf, ranges)["data"])

    # Code pointers another image holds into this one: the executable's table of a code
    # overlay's entry points. Several overlays share one address range, so a pointer seeds
    # this image only where its own word there is entry-shaped, as for an address
    # materialized in code.
    for val in entry_hints or ():
        if in_text(val) and _is_address_taken_entry(elf, val):
            hc.add(val)

    # A computed `jr` whose target set the code bounds (an `andi` mask applied to a
    # materialized base) lands only on those pads; each one needs a dispatch entry.
    hc.update(bounded_computed_goto_targets(elf, ranges))


def _seed_address_materializations(d):
    elf, ranges, file_exec_ranges = d.elf, d.ranges, d.file_exec_ranges
    relocated, jumps, hc, noisy, jtails = d.relocated, d.jumps, d.hc, d.noisy, d.jtails
    cfg_gate, in_text = d.cfg_gate, d.in_text
    # Sweep executable sections for weaker "block boundary" signals and possible indirect
    # targets. Direct JAL targets are collected by trace_function only after their source
    # instruction is reached; a linear sweep can mistake data inside .text for a call.
    sys.stderr.write(f"SCANNING RANGES: {ranges}\n")

    def note_address_taken(val, low_half_addr):
        if (val & 3) != 0 or not _relocated_as(relocated, low_half_addr, "lo16"):
            return
        if in_ranges(val, ranges):
            if _is_address_taken_entry(elf, val):
                hc.add(val)
            else:
                noisy.add(val)
        elif in_ranges(val, file_exec_ranges) and _is_address_taken_entry(elf, val):
            # Executable PT_LOAD bytes outside the named code sections (.init, .fini)
            # are already callable when a `jal` names them; a materialized pointer to
            # an entry-shaped word there (e.g. a `.fini` routine registered for exit)
            # is the same evidence.
            hc.add(val)

    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            continue
        hireg = {}
        # A control transfer ends the straight-line run after its delay slot, which
        # still executes first: an `la` whose low half sits in a `jal` delay slot
        # (`lui a1, hi; ...; jal f; addiu a1, a1, lo`) is a pointer argument. Drop
        # the state at the word after the delay slot, keeping what the slot wrote.
        clear_at = None
        before_slot = None
        for off in range(0, len(blob) - 3, 4):
            word = int.from_bytes(blob[off:off + 4], 'little')
            addr = lo + off
            if clear_at is not None and addr == clear_at - 4:
                before_slot = dict(hireg)
            elif clear_at is not None and addr == clear_at:
                hireg = {
                    reg: value for reg, value in hireg.items()
                    if before_slot is not None
                    and (reg not in before_slot or before_slot[reg] != value)
                }
                clear_at = before_slot = None
            op = word >> 26
            if op == 2:  # j: tail call or intra-function goto -> weak
                target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges) and _relocated_as(jumps, addr, "jump26"):
                    noisy.add(target)
                    jtails.add(target)
            if (word >> 16) == 0x27BD and (word & 0x8000):  # addiu $sp,$sp,-N prologue
                noisy.add(addr)
            # The opt-in ownership gate does not treat a direct `j` as having
            # a sequential successor after its delay slot. Keep the legacy
            # discovery heuristic when the gate is off so default output stays
            # byte-identical.
            is_uncond = ((not cfg_gate and op == 2)
                         or (op == 0 and (word & 0x3F) == 0x08)
                         or (op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0))
            if is_uncond and in_ranges(addr + 8, ranges):
                tb = elf.read_at_vaddr(addr + 8, 4)
                is_jr_ra = tb is not None and len(tb) == 4 and int.from_bytes(tb, 'little') == 0x03e00008
                if is_jr_ra:
                    if op == 0 and (word & 0x3F) == 0x08:
                        # Trailing return-epilogue: the function returns at `addr`, and the
                        # `jr $ra` immediately after its delay slot (addr+8) is a hoisted /
                        # outlined epilogue stub (e.g. `return a0`), not a separate function.
                        # trace_function already folds addr+8 into the preceding function's
                        # covered set, so keep it only as a weak signal; gap-fill skips it
                        # because it is already covered by that function.
                        noisy.add(addr + 8)
                    else:
                        hc.add(addr + 8)
                else:
                    noisy.add(addr + 8)

            is_branch = op in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (op == 0x11 and ((word >> 21) & 0x1F) == 8)
            is_jump_or_call = op in (2, 3) or (op == 0 and (word & 0x3F) in (0x08, 0x09))
            if (is_branch or is_jump_or_call) and clear_at is None:
                clear_at = addr + 8

            if op == 0x0F:  # lui rt, imm
                hireg[(word >> 16) & 0x1F] = (word & 0xFFFF) << 16
            elif op == 0x09:  # addiu rt, rs, simm  (la low half)
                rs = (word >> 21) & 0x1F
                if rs in hireg and isinstance(hireg[rs], int):
                    val = (hireg[rs] + ((word & 0xFFFF) - (0x10000 if word & 0x8000 else 0))) & 0xFFFFFFFF
                    note_address_taken(val, addr)
            elif op == 0x0D:  # ori rt, rs, imm  (la low half)
                rs = (word >> 21) & 0x1F
                if rs in hireg and isinstance(hireg[rs], int):
                    val = hireg[rs] | (word & 0xFFFF)
                    note_address_taken(val, addr)
            elif op == 0 and (word & 0x3F) == 0x21: # addu rd, rs, rt
                rs_reg, rt_reg, rd_reg = (word >> 21) & 0x1F, (word >> 16) & 0x1F, (word >> 11) & 0x1F
                if rs_reg in hireg and isinstance(hireg[rs_reg], int):
                    hireg[rd_reg] = hireg[rs_reg]
                elif rt_reg in hireg and isinstance(hireg[rt_reg], int):
                    hireg[rd_reg] = hireg[rt_reg]
            elif op == 0x23:  # lw rt, simm(rs)
                rs_reg = (word >> 21) & 0x1F
                if rs_reg in hireg and isinstance(hireg[rs_reg], int):
                    table_addr = (hireg[rs_reg] + ((word & 0xFFFF) - (0x10000 if word & 0x8000 else 0))) & 0xFFFFFFFF
                    hireg[(word >> 16) & 0x1F] = ("table", table_addr)
            elif op == 0x2B:  # sw rt, simm(rs)
                rt_reg = (word >> 16) & 0x1F
                if rt_reg in hireg and isinstance(hireg[rt_reg], int):
                    ptr = hireg[rt_reg]
                    # Heuristic: if we store a pointer that itself points to code, it's likely a vtable.
                    # This catches C++ constructors setting up the vtable pointer.
                    tb = elf.read_at_vaddr(ptr, 4)
                    if tb and len(tb) == 4:
                        val = int.from_bytes(tb, 'little')
                        if in_text(val):
                            for j in range(512):
                                tb = elf.read_at_vaddr(ptr + j*4, 4)
                                if not tb or len(tb) < 4: break
                                val = int.from_bytes(tb, 'little')
                                if in_text(val) and _relocated_as(relocated, ptr + j*4, "word32"):
                                    hc.add(val)
                                else: break
            elif op == 0 and (word & 0x3F) in (0x08, 0x09):  # jr rs / jalr rd, rs
                rs_reg = (word >> 21) & 0x1F
                if rs_reg in hireg and isinstance(hireg[rs_reg], tuple) and hireg[rs_reg][0] == "table":
                    table_addr = hireg[rs_reg][1]
                    for j in range(512): # Scan up to 512 entries
                        tb = elf.read_at_vaddr(table_addr + j*4, 4)
                        if not tb or len(tb) < 4: break
                        val = int.from_bytes(tb, 'little')
                        if in_text(val) and _relocated_as(relocated, table_addr + j*4, "word32"):
                            hc.add(val)
                        else: break


def _promote_tail_calls(d):
    """Return the calls of the last promoted tail-call body, for the gap fill to resolve."""
    elf, jtails, functions = d.elf, d.jtails, d.functions
    callable_target, trace, trace_ranges_for_entry = d.callable_target, d.trace, d.trace_ranges_for_entry
    # Tail-call promotion: a `j` target that no fallthrough can reach (the word 8
    # bytes before it -- the previous slot's terminator position -- is `jr`, `j`,
    # or an unconditional `b`) is a function entered only by jumps: the shared-
    # return / compiler-split cold-path / trampoline idiom. Left unpromoted,
    # whichever function linearly covers the target absorbs its body, and every
    # OTHER function's tail `j` has no entry to dispatch to at runtime -- a
    # silent NONPLT_MISS of the 0x000e1724 class (found on 2026-07-18 by
    # tools/ghidra_crosscheck.py, 16 live cases). Promotion is safe: codegen's
    # continuation machinery stops the covering function's extent at the new
    # entry and emits a continuation call, so both owners stay correct.
    pending_tail_calls = []
    for t in sorted(jtails):
        if t in functions or not callable_target(t):
            continue
        wb = elf.read_at_vaddr(t - 8, 4)
        if wb is None or len(wb) < 4:
            continue
        if not _is_hard_terminator(int.from_bytes(wb, 'little')):
            continue
        functions.add(t)
        pending_tail_calls.extend(trace(t, trace_ranges_for_entry(t)))
        tail_call_batch = pending_tail_calls
        pending_tail_calls = []
        for c in tail_call_batch:
            if c not in functions and callable_target(c):
                functions.add(c)
                pending_tail_calls.extend(trace(c, trace_ranges_for_entry(c)))
    return pending_tail_calls


def _fill_gaps(d, pending_tail_calls):
    elf, ranges, noisy, covered, functions = d.elf, d.ranges, d.noisy, d.covered, d.functions
    callable_target, trace, trace_ranges_for_entry = d.callable_target, d.trace, d.trace_ranges_for_entry
    # Gap fill: a weak-signal address that no known function covers is an indirect-only
    # function (reached through a register the call graph could not resolve). Add it and trace
    # it, which may reveal further calls. Iterate until stable.
    pending_gap_calls = pending_tail_calls
    changed = True
    while changed:
        changed = False
        for c in sorted(noisy):
            if in_ranges(c, ranges) and c not in covered and c not in functions:
                # Safety net: if a candidate address immediately follows a
                # covered instruction AND starts with jr $ra, it is a thin
                # return stub that belongs to the preceding function (e.g. a
                # comparator callback embedded in a function's address range).
                # Skip it rather than creating a spurious standalone function.
                if (c - 4) in covered or (c - 8) in covered:
                    wb = elf.read_at_vaddr(c, 4)
                    if wb and len(wb) == 4 and int.from_bytes(wb, 'little') == 0x03e00008:
                        covered.add(c)
                        covered.add(c + 4)
                        continue
                # Reverse safety net: if a candidate address is immediately
                # BEFORE a covered instruction AND starts with jr $ra, it is
                # a tail stub that belongs to the preceding function. Skip it
                # rather than creating a spurious standalone function.
                if (c + 4) in covered or (c + 8) in covered:
                    wb = elf.read_at_vaddr(c, 4)
                    if wb and len(wb) == 4 and int.from_bytes(wb, 'little') == 0x03e00008:
                        covered.add(c)
                        covered.add(c + 4)
                        continue
                functions.add(c)
                new_calls = trace(c, ranges)
                gap_call_batch = pending_gap_calls + new_calls
                pending_gap_calls = []
                for t in gap_call_batch:
                    if t not in functions and callable_target(t):
                        functions.add(t)
                        pending_gap_calls.extend(trace(t, trace_ranges_for_entry(t)))
                changed = True


def _owned_spans(d):
    """The executable ranges plus the instructions the traces reached outside them, merged."""
    elf, ranges, file_exec_ranges = d.elf, d.ranges, d.file_exec_ranges
    covered, proven_spans = d.covered, d.proven_spans
    # A direct call, or a called pointer array, can prove that code exists outside named
    # .text sections. Grant the interpreter floor only the instructions reached from those
    # roots, never the surrounding executable PT_LOAD's unowned gaps.
    extra_addresses = sorted({
        address for address in covered
        if not in_ranges(address, ranges)
        and (in_ranges(address, file_exec_ranges) or in_ranges(address, proven_spans))
        and elf.read_at_vaddr(address, 4) is not None
    })
    owned_spans = list(ranges)
    if extra_addresses:
        span_start = extra_addresses[0]
        span_end = span_start + 4
        for address in extra_addresses[1:]:
            if address == span_end:
                span_end += 4
            else:
                owned_spans.append((span_start, span_end))
                span_start, span_end = address, address + 4
        owned_spans.append((span_start, span_end))
    owned_spans.sort()
    merged_ranges = []
    for lo, hi in owned_spans:
        if merged_ranges and lo <= merged_ranges[-1][1]:
            merged_ranges[-1] = (merged_ranges[-1][0], max(merged_ranges[-1][1], hi))
        else:
            merged_ranges.append((lo, hi))
    return merged_ranges


def analyze(elf, extra_spans=None, cfg_gate=False, trace_memo=True, report=None,
            entry_hints=None):
    # trace_memo=False runs the uncached per-start walks; the output is the same either way
    # (tests pin the two against each other).
    # `entry_hints` are code pointers another image holds into this one (data_section_words).
    # `report`, when a dict, receives the discovery state the import-stub census reads:
    # covered (every traced instruction address), calls (every direct-call target seen),
    # and hc (the high-confidence seeds). The return value is unchanged.
    d = _Discovery(elf, extra_spans, cfg_gate, trace_memo)
    _seed_entry_point(d)
    _seed_prologues_and_terminators(d)
    _seed_module_exports(d)
    _seed_import_stubs(d)
    _seed_function_pointer_tables(d, entry_hints)
    _seed_address_materializations(d)

    # Trace each function's extent from the high-confidence seeds, following discovered calls.
    d.functions = set(d.hc)
    d.discover(list(d.hc))
    pending_tail_calls = _promote_tail_calls(d)
    _fill_gaps(d, pending_tail_calls)

    # The late passes can reach start-up code that calls a pointer array (a function
    # entered only through a pointer or the gap fill); seed those arrays too.
    d.discover([])

    merged_ranges = _owned_spans(d)
    if report is not None:
        report.update(covered=d.covered, calls=d.calls, hc=d.hc)
    return d.functions, merged_ranges


# ---- TOML model: a function inventory the codegen reads and a human can correct ----------

def build_model(elf, starts):
    stub = elf.sec(".sceStub.text")

    def kind(a):
        if stub and stub["addr"] <= a < stub["addr"] + stub["size"]:
            return "import_stub"
        return "function"

    return {
        "module": (elf.sec(".rodata.sceModuleInfo") is not None) and "prx" or "elf",
        "entry": int(elf.entry) & 0xFFFFFFFF,
        "functions": [{"addr": int(a) & 0xFFFFFFFF, "kind": kind(a)} for a in sorted(starts)],
    }


# Per-function fields are emitted in this order; unknown extra fields are appended so a
# hand-corrected TOML round-trips without loss.
_FUNC_ORDER = ["addr", "kind", "name", "skip", "note"]


def _fmt_value(key, val):
    if key == "addr" or (key == "entry"):
        # Mask to a guest 32-bit address before formatting. Unrelocated PRX entries
        # come in as 0x1..XXXXXXXX (the kernel module base bit, 0x100000000, leaks
        # through); without the mask the TOML prints >32-bit hex and the loader/roundtrip
        # comparisons treat the value as 0x1XXXXXXXX on the next pass.
        return f"0x{int(val) & 0xFFFFFFFF:08x}"
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, int):
        return str(val)
    text = str(val)
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in text):
        raise ValueError(f"cannot emit control characters into TOML: {key!r}")
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_toml(model, path):
    lines = [
        "# Function inventory emitted by tools/analyze.py. Machine-generated, human-editable.",
        "# Edit names/skip/note or add functions; the loader round-trips edits without loss.",
        f'module = {_fmt_value("module", model["module"])}',
        f'entry = {_fmt_value("entry", model["entry"])}',
        "",
    ]
    functions = sorted(
        model["functions"],
        key=lambda fn: (
            isinstance(fn.get("addr"), (int, float)),
            int(fn.get("addr")) if isinstance(fn.get("addr"), (int, float)) else 0,
            str(fn.get("addr", "")),
        ),
    )
    for fn in functions:
        lines.append("[[function]]")
        extra_keys = sorted(k for k in fn if k not in _FUNC_ORDER)
        keys = [k for k in _FUNC_ORDER if k in fn] + extra_keys
        for k in keys:
            lines.append(f"{k} = {_fmt_value(k, fn[k])}")
        lines.append("")
    import os
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))


def load_toml(path):
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    return {
        "module": doc.get("module", "elf"),
        "entry": doc.get("entry", 0),
        "functions": doc.get("function", []),
    }


def roundtrip_check(model, workdir):
    # Emit the model, simulate a hand correction (rename one function, skip another, add a
    # note), then load -> re-emit -> load and require the corrections and the full function
    # set to survive byte-for-byte.
    import os
    os.makedirs(workdir, exist_ok=True)
    t1 = os.path.join(workdir, "functions.toml")
    emit_toml(model, t1)

    corrected = load_toml(t1)
    if len(corrected["functions"]) >= 2:
        corrected["functions"][0]["name"] = "module_entry"
        corrected["functions"][1]["skip"] = True
        corrected["functions"][1]["note"] = "hand-corrected: handled by HLE"
    tc = os.path.join(workdir, "functions.corrected.toml")
    emit_toml(corrected, tc)

    # Idempotence: loading and re-emitting the corrected TOML must be byte-identical.
    reloaded = load_toml(tc)
    tc2 = os.path.join(workdir, "functions.corrected.2.toml")
    emit_toml(reloaded, tc2)
    a = open(tc, "rb").read()
    b = open(tc2, "rb").read()
    ok = a == b
    # And the corrections must still be present after the round-trip.
    fn0, fn1 = reloaded["functions"][0], reloaded["functions"][1]
    preserved = fn0.get("name") == "module_entry" and fn1.get("skip") is True and "note" in fn1
    same_count = len(reloaded["functions"]) == len(model["functions"])
    return ok and preserved and same_count, t1


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]
    if not args:
        sys.stderr.write(
            "usage: analyze.py <elf> [--base=HEX] [--extra-span=LO,HI] "
            "[--toml=out.toml] [--check=workdir] "
            "[--cfg-report=out.json] [--cfg-gate] [--quiet]\n"
        )
        return 2
    base = None
    extra_span_arg = None
    cfg_report_path = None
    cfg_gate = False
    for o in opts:
        if o.startswith("--base="):
            base = int(o.split("=", 1)[1], 16)
        elif o.startswith("--extra-span="):
            extra_span_arg = o.split("=", 1)[1]
        elif o.startswith("--cfg-report="):
            cfg_report_path = o.split("=", 1)[1]
            if not cfg_report_path:
                sys.stderr.write("--cfg-report requires a path\n")
                return 2
        elif o == "--cfg-gate":
            cfg_gate = True
    elf = Elf(args[0], base=base)
    extra_spans = resolve_extra_spans(extra_span_arg)
    try:
        starts, ranges = analyze(elf, extra_spans=extra_spans, cfg_gate=cfg_gate)
    except Exception as exc:
        boundary_code = getattr(exc, "code", None)
        if not isinstance(boundary_code, str) or not boundary_code.startswith("ANALYZER_"):
            raise
        message = "This PSP import-table layout is not supported yet."
        sys.stderr.write(f"{boundary_code}: {exc}. {message}\n")
        return 1
    model = build_model(elf, starts)

    quiet = "--quiet" in opts
    rc = 0
    if cfg_report_path is not None or cfg_gate:
        report_entries, report_ranges = starts, ranges
        if not cfg_gate:
            # A report-only run describes gate-mode analysis but must not change
            # the entry set used for the model and recall output below.
            report_entries, report_ranges = analyze(elf, extra_spans=extra_spans, cfg_gate=True)
        cfg_report = canonical_cfg_report(elf, ranges=report_ranges, entries=report_entries)
        if cfg_report_path is not None:
            parent = os.path.dirname(cfg_report_path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(cfg_report_path, "w", encoding="ascii", newline="\n") as output:
                output.write(canonical_cfg_json(cfg_report))
            print("wrote CFG report:", cfg_report_path)
        if cfg_gate:
            cfg_findings = canonical_cfg_gate(cfg_report)
            if cfg_findings:
                for finding in cfg_findings:
                    sys.stderr.write(
                        f"CFG_GATE {finding['code']}: {finding['message']}\n"
                    )
                rc = 1
            else:
                print(
                    "CFG_GATE PASS: "
                    f"{len(cfg_report['instructions'])} executable words, "
                    f"{len(cfg_report['entries'])} entries"
                )
    truth = elf.func_symbols()
    if truth is not None:
        truth_in = set(a for a in truth if in_ranges(a, ranges))
        found = truth_in & starts
        recall = len(found) / len(truth_in) if truth_in else 1.0
        missed = sorted(truth_in - starts)
        if not quiet:
            print(f"ground-truth functions (in exec ranges): {len(truth_in)}")
            print(f"discovered entry points: {len(starts)}")
            print(f"recovered: {len(found)}  recall: {recall*100:.2f}%")
            if missed:
                print(f"missed {len(missed)} (first 20): " + ", ".join("0x%08x" % a for a in missed[:20]))
        print(f"RECALL {recall*100:.2f}% ({len(found)}/{len(truth_in)})")
        if recall < 0.95:
            rc = 1

    for o in opts:
        if o.startswith("--toml="):
            emit_toml(model, o.split("=", 1)[1])
            print("wrote TOML:", o.split("=", 1)[1])
        if o.startswith("--check="):
            ok, t1 = roundtrip_check(model, o.split("=", 1)[1])
            print(f"TOML round-trip (hand-corrected, lossless): {'OK' if ok else 'FAIL'}")
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    # Run as a script this file is `__main__`, and helpers it imports lazily (entry_frame_balance
    # for linked branches) import it again as `analyze`, which would load a second copy with its
    # own _SpanIndex class: a span index built by one copy then fails the other's isinstance test.
    # One module object serves both names.
    sys.modules.setdefault("analyze", sys.modules[__name__])
    sys.exit(main(sys.argv))
