# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Safe, bounded ISO9660 and PARAM.SFO inspector for PSP disc images."""

from __future__ import annotations

from collections import deque
import hashlib
import json
import os
from dataclasses import dataclass
import functools
from pathlib import Path
import struct
from typing import Dict, Iterator, Optional, Sequence

import psp_import_table
import title_manifest
from . import disc_expand_tool
from .title_registry import TitleRegistry, get_default_registry
from .types import IsoMetadata


SECTOR_SIZE = 2048
PVD_SECTOR = 16
ISO_MAGIC = b"\x01CD001\x01"
SFO_MAGIC = b"\x00PSF\x01\x01\x00\x00"
MAX_DIRECTORY_BYTES = 512 * 1024
# A directory record is at least 34 bytes, so MAX_DIRECTORY_BYTES is also the bound on
# how many entries one directory can list; no separate entry cap applies (a disc's
# speech or data folder can list several thousand files).
MAX_SFO_BYTES = 64 * 1024
MAX_EXECUTABLE_BYTES = 512 * 1024 * 1024
MAX_CFW_EBOOT_SCAN_BYTES = 256 * 1024
EXPERIMENTAL_PROFILE_SCHEMA_VERSION = 1
PSP_DEFAULT_MAIN_LOAD_ADDRESS = 0x08804000
PSP_CONVENTIONAL_USER_MEMORY_TOP = 0x0A000000
SFO_FMT_UTF8_SPECIAL = 0x0004
SFO_FMT_UTF8 = 0x0204
SFO_FMT_UINT32 = 0x0404
PBP_MAGIC = b"\x00PBP"
PBP_HEADER_SIZE = 40
PBP_SECTION_COUNT = 8
PBP_PARAM_SFO_SECTION = 0
PBP_TITLE_MAX_BYTES = 128  # same bound as the native title_name field
PBP_PACKAGE_UNSUPPORTED = "PBP_PACKAGE_UNSUPPORTED"
PBP_HEADER_TRUNCATED = "PBP_HEADER_TRUNCATED"
PBP_OFFSETS_INVALID = "PBP_OFFSETS_INVALID"
PBP_SFO_INVALID = "PBP_SFO_INVALID"
# The single registry of identify refusals this module emits. Every raise goes
# through _pbp_refusal, which accepts only these codes. The parity tests derive
# their expected sets from this tuple, so the C emitter, the bring-up schema enum,
# and the sweep vocabulary are all checked against the same source.
PBP_BOUNDARY_CODES: tuple[str, ...] = (
    PBP_PACKAGE_UNSUPPORTED,
    PBP_HEADER_TRUNCATED,
    PBP_OFFSETS_INVALID,
    PBP_SFO_INVALID,
)
# The command line keeps refusing packages; the player converts a PSN release
# of a UMD game into its disc image (#295 phase 2) and imports that instead.
PBP_UNSUPPORTED_SENTENCE = (
    "This is a PlayStation Store package (PBP), not a disc image. "
    "The player converts PSN UMD games."
)
PBP_HEADER_TRUNCATED_SENTENCE = (
    "This file starts like a PlayStation Store package (PBP) but ends "
    "before the 40-byte package header is complete."
)
PBP_OFFSETS_INVALID_SENTENCE = (
    "This PlayStation Store package (PBP) has a section table that is not "
    "ascending or that points past the end of the file."
)
PBP_SFO_INVALID_SENTENCE = (
    "The title information (PARAM.SFO) inside this PlayStation Store "
    "package (PBP) is missing, oversized, or malformed."
)


class IsoInspectionError(ValueError):
    """Raised when an ISO image is unreadable or malformed.

    ``boundary_code`` names a format boundary the file was identified as
    (for example a PlayStation Store package Nakagawa cannot use). It is
    carried beside the message, never inside it: ``str(exc)`` stays the
    plain sentence shown to a person.
    """

    def __init__(self, message: str, *, boundary_code: str | None = None) -> None:
        super().__init__(message)
        self.boundary_code = boundary_code


def _has_cfw_or_kernel_only_imports(elf_bytes: bytes) -> bool:
    """Return whether a validated ELF import table names a CFW-only library."""
    try:
        from analyze import Elf
        from imports import parse_imports

        imports = parse_imports(Elf(elf_bytes))
    except (ImportError, IndexError, OSError, TypeError, ValueError, struct.error):
        return False
    cfw_libraries = {"systemctrlforkernel", "kubridge"}
    return any(
        library.casefold() in cfw_libraries or library.casefold().endswith("forkernel")
        for library, _nid in imports.values()
    )


class RuntimeRegistryUnavailableError(IsoInspectionError):
    """The runtime HLE registry (src/rt/hle.c) could not be read.

    Guest-module planning needs the exact set of NIDs the runtime registers to
    decide which disc modules the runtime itself provides; without it there is
    no correct decision, so planning stops with a named boundary instead of
    guessing that nothing (or everything) is served.
    """

    def __init__(self, message: str) -> None:
        super().__init__(message, boundary_code="RUNTIME_HLE_REGISTRY_UNAVAILABLE")


@functools.lru_cache(maxsize=1)
def runtime_registered_nids() -> frozenset[int]:
    """Every function NID the runtime registers, from its single source of truth.

    The set comes from tools/hle_manifest.registered_nids(), the fail-closed
    extraction of src/rt/hle.c that the HLE manifest and the import audit gate
    use. A failure raises RuntimeRegistryUnavailableError; only a successful
    read is cached.
    """
    try:
        import hle_manifest
    except ImportError as exc:
        raise RuntimeRegistryUnavailableError(
            f"the runtime HLE registry reader could not be loaded: {exc}"
        ) from exc
    try:
        return hle_manifest.registered_nids()
    except (OSError, ValueError, hle_manifest.ManifestError) as exc:
        raise RuntimeRegistryUnavailableError(
            f"the runtime HLE registry (src/rt/hle.c) could not be read: {exc}"
        ) from exc


@dataclass(frozen=True)
class GuestModuleInterface:
    """What one disc module offers the title, read from its PSP module tables.

    ``callable_nids`` and ``callable_variables`` cover only the libraries a
    user-mode importer can link against (psp_import_table.ExportTable.
    user_callable_libraries). ``requires_kernel`` is true for a module whose
    SceModuleInfo carries the kernel-mode attribute or which imports
    CFW/kernel-only libraries; neither can run as translated user-mode code.
    """

    requires_kernel: bool
    callable_nids: frozenset[int]
    callable_variables: int


def read_guest_module_interface(name: str, module_bytes: bytes) -> GuestModuleInterface:
    """Read a plain disc module's export interface, failing closed by name.

    An encrypted container is GUEST_MODULE_DECRYPTION_REQUIRED and a malformed
    module or export table is GUEST_MODULE_FORMAT_UNSUPPORTED. An image that
    declares no SceModuleInfo at all (no module-info section and a zero
    phdr[0].p_paddr) exports nothing and carries no kernel-mode attribute, so
    it is a user-mode module with an empty interface. The CFW/kernel-only
    import check reads the import table with the analyzer's model; a table it
    cannot read names no such library here, and code generation refuses that
    module by its import-table boundary when it translates it.
    """
    if module_bytes[:4] in (b"~PSP", b"~SCE"):
        raise IsoInspectionError(
            f"guest module is an encrypted container (~PSP/~SCE) and needs decryption: {name}",
            boundary_code="GUEST_MODULE_DECRYPTION_REQUIRED",
        )
    try:
        table = psp_import_table.parse_export_table(module_bytes)
    except psp_import_table.ImportTableError as exc:
        raise IsoInspectionError(
            f"guest module has an invalid module or export table: {name}: {exc}",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        ) from exc
    if table is None:
        return GuestModuleInterface(False, frozenset(), 0)
    libraries = table.user_callable_libraries()
    return GuestModuleInterface(
        requires_kernel=table.kernel_mode or _has_cfw_or_kernel_only_imports(module_bytes),
        callable_nids=frozenset(nid for library in libraries for nid in library.function_nids),
        callable_variables=sum(len(library.variable_nids) for library in libraries),
    )


def runtime_serves_module(interface: GuestModuleInterface, registered: frozenset[int]) -> bool:
    """Whether the runtime replaces this module completely.

    The threshold is every exported function a user-mode importer can call:
    the module is runtime-served when it exports at least one such function,
    the runtime registers every one of those NIDs, and it exports no variable
    to user mode (the runtime registers functions only). A module whose
    callable exports are partly registered keeps its own code for the rest,
    so it is not served; a module that exports nothing callable (for example
    one that only runs from module_start) is never served.
    """
    return (
        bool(interface.callable_nids)
        and interface.callable_variables == 0
        and interface.callable_nids <= registered
    )


def _main_executable_imported_nids(main_elf: Path | str) -> frozenset[int]:
    """Every function NID the main executable imports, as code generation reads it.

    Uses the analyzer's import model (tools/imports.parse_imports), so the set
    is exactly what the translated main executable calls. As in the analyzer,
    an executable without SceModuleInfo imports nothing; an import table the
    analyzer would refuse stops planning with the analyzer's own boundary.
    """
    from analyze import Elf
    from imports import ImportTableError, parse_imports

    try:
        elf = Elf(str(main_elf))
    except (OSError, ValueError, struct.error) as exc:
        raise IsoInspectionError(
            f"main executable could not be read for its import table: {exc}",
            boundary_code="ANALYZER_IMPORT_TABLE_INVALID",
        ) from exc
    if elf.sec(".rodata.sceModuleInfo") is None:
        return frozenset()
    try:
        return frozenset(nid for _library, nid in parse_imports(elf).values())
    except ImportTableError as exc:
        raise IsoInspectionError(
            f"main executable import table is not supported: {exc}",
            boundary_code=exc.code,
        ) from exc


@dataclass(frozen=True)
class IsoDirectoryEntry:
    """One bounded ISO9660 directory entry with its validated extent."""

    name: str
    lba: int
    size: int
    is_directory: bool
    multi_extent: bool


def _elf32_load_span_from_reader(read, file_size: int) -> tuple[int, int, int]:
    """Return (ELF type, lowest PT_LOAD address, highest PT_LOAD end)."""
    header = read(0, 52)
    if len(header) < 52:
        if header[:4] in (b"~PSP", b"~SCE"):
            raise IsoInspectionError(
                "Guest module is an encrypted container (~PSP/~SCE) requiring decryption",
                boundary_code="GUEST_MODULE_DECRYPTION_REQUIRED",
            )
        raise IsoInspectionError(
            "ELF32 image is truncated",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )
    if header[:7] != b"\x7fELF\x01\x01\x01":
        if header[:4] in (b"~PSP", b"~SCE"):
            raise IsoInspectionError(
                "Guest module is an encrypted container (~PSP/~SCE) requiring decryption",
                boundary_code="GUEST_MODULE_DECRYPTION_REQUIRED",
            )
        raise IsoInspectionError(
            "ELF32 load binding needs a little-endian ELF32 image",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )
    e_type, machine, version = struct.unpack_from("<HHI", header, 16)
    _entry, phoff = struct.unpack_from("<II", header, 24)
    ehsize, phentsize, phnum = struct.unpack_from("<HHH", header, 40)
    if machine != 8 or version != 1 or ehsize != 52 or phentsize != 32:
        raise IsoInspectionError(
            "ELF32 load binding needs a supported MIPS program-header table",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )
    if not 1 <= phnum <= 128 or phoff < ehsize:
        raise IsoInspectionError(
            "ELF32 load binding has an unsupported program-header count",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )
    ph_end = phoff + phentsize * phnum
    if ph_end < phoff or ph_end > file_size:
        raise IsoInspectionError(
            "ELF32 program headers exceed the input image",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )
    table = read(phoff, phentsize * phnum)
    if len(table) != phentsize * phnum:
        raise IsoInspectionError(
            "ELF32 program headers are truncated",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )

    segments: list[tuple[int, int]] = []
    low: int | None = None
    high = 0
    for index in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _flags, _align = \
            struct.unpack_from("<8I", table, index * phentsize)
        if p_offset + p_filesz < p_offset or p_offset + p_filesz > file_size:
            raise IsoInspectionError(
                "ELF32 segment file range exceeds the input image",
                boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
            )
        if p_type != 1:
            continue
        end = p_vaddr + p_memsz
        if p_memsz < p_filesz or end < p_vaddr or end > 0xFFFFFFFF:
            raise IsoInspectionError(
                "ELF32 segment memory range is invalid",
                boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
            )
        segments.append((p_vaddr, end))
        low = p_vaddr if low is None else min(low, p_vaddr)
        high = max(high, end)
    if low is None or high <= low:
        raise IsoInspectionError(
            "ELF32 image has no non-empty loadable segment",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        )
    segments.sort(key=lambda seg: seg[0])
    for i in range(len(segments) - 1):
        if segments[i][1] > segments[i + 1][0]:
            raise IsoInspectionError(
                "ELF32 image has overlapping loadable segments",
                boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
            )
    return e_type, low, high


def _elf32_load_span(path: Path | str) -> tuple[int, int, int]:
    """Read a filesystem ELF and return its PT_LOAD span."""
    try:
        image_path = Path(path)
        file_size = image_path.stat().st_size
        with image_path.open("rb") as stream:
            def read(offset: int, count: int) -> bytes:
                stream.seek(offset)
                return stream.read(count)

            return _elf32_load_span_from_reader(read, file_size)
    except OSError as exc:
        raise IsoInspectionError(
            "ELF32 image could not be read for guest-module placement",
            boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
        ) from exc


def plan_guest_module_bindings(
    main_elf: Path | str,
    module_inputs: Sequence[tuple[str, Path | str, str]],
) -> list[dict]:
    """The declared guest modules of plan_guest_module_layout (deferred items omitted)."""
    return plan_guest_module_layout(main_elf, module_inputs)[0]


def plan_guest_module_layout(
    main_elf: Path | str,
    module_inputs: Sequence[tuple[str, Path | str, str]],
) -> tuple[list[dict], list[dict]]:
    """Declare each translated guest module placed by the guest allocator at run time.
    Returns (declared modules, deferred items). A deferred item is named and carries its
    reason; it is not declared and not placed at boot: a byte-identical copy of the main
    executable is recorded as a duplicate, and a fixed-address ET_EXEC image that lies on
    the main image is a separate executable, not pre-placed beside main (whether the
    title loads it later is not established).

    The PSP kernel's loader takes a module's memory from the user partition when the
    game loads it, so the build reserves nothing: modules need not fit together, and a
    load that finds no room fails at run time with the status the kernel reports
    (#704). Each declared module is translated position-independently (``placement:
    runtime``). Only what is fixed before the game runs is checked here: the main image
    must lie inside user memory (GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE).

    Only modules that run as translated guest code are declared. Each module's export
    table is compared, NID by NID, with the NIDs the runtime registers
    (runtime_registered_nids):

    * a module the runtime replaces completely (runtime_serves_module) is not
      declared and not translated: the runtime answers every function it exports;
    * a kernel-mode module is never declared, because the runtime does not run PSP
      kernel-mode code. It stops planning with GUEST_MODULE_FORMAT_UNSUPPORTED only
      when the main executable imports one of its user-callable functions that
      neither the runtime registers nor a declared module exports: the title's own
      program then depends on kernel code nothing provides. Without such an import
      (for example a firmware driver that only another firmware library calls) the
      module is left out, and a call into it from a library that does run fails
      closed at the call as a named unimplemented NID;
    * every other module is declared. A relocatable module (ELF type 0xFFA0 or 3)
      goes wherever the allocator puts it. A fixed-address module (ELF type 2) can
      load only at its link address, so that range must lie inside user memory
      without overlapping the main image (GUEST_MODULE_LOAD_BINDING_REQUIRED
      otherwise). Two fixed-address modules may share addresses: only modules loaded
      at the same time compete for memory, and the allocator decides that when the
      game loads them.
    * a code overlay (an MWo3 file the game reads into memory itself) is declared the
      same way and translated at its own load address; the runtime binds it when the
      game invalidates the instruction cache over it, not at sceKernelLoadModule.

    Modules are listed by case-insensitive filename.
    """
    main_type, main_low, main_high = _elf32_load_span(main_elf)
    if main_type in (3, 0xFFA0):
        main_start = PSP_DEFAULT_MAIN_LOAD_ADDRESS + main_low
        main_end = PSP_DEFAULT_MAIN_LOAD_ADDRESS + main_high
    elif main_type == 2:
        main_start, main_end = main_low, main_high
    else:
        raise IsoInspectionError(
            "main executable type has no supported guest-module layout",
            boundary_code="GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE",
        )
    if main_end > PSP_CONVENTIONAL_USER_MEMORY_TOP:
        raise IsoInspectionError(
            "main executable exceeds the conventional user-memory ceiling",
            boundary_code="GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE",
        )

    modules: list[dict] = []
    deferred: list[dict] = []
    kernel_modules: list[tuple[str, GuestModuleInterface]] = []
    placed_exports: set[int] = set()
    folded_names: set[str] = set()
    try:
        main_bytes = Path(main_elf).read_bytes()
    except OSError as exc:
        raise IsoInspectionError(
            "main executable could not be read for guest-module planning",
            boundary_code="GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE",
        ) from exc
    for name, module_path, guest_path in module_inputs:
        folded = name.casefold()
        if folded in folded_names:
            raise IsoInspectionError(
                f"guest-module filenames collide under the placement policy: {name}",
                boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
            )
        folded_names.add(folded)
        try:
            mod_bytes = Path(module_path).read_bytes()
        except OSError as exc:
            raise IsoInspectionError(
                f"guest module could not be read: {name}",
                boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
            ) from exc
        if mod_bytes == main_bytes:
            deferred.append({
                "name": name,
                "reason": "duplicate of the main executable, not loaded as a module",
            })
            continue
        if mod_bytes[:4] == b"MWo3":
            # A code overlay is not a module: the game reads it to its own load address
            # and announces it with an instruction-cache invalidate, where the runtime
            # binds its translation. It is translated like a runtime-placed module and
            # never loaded through sceKernelLoadModule.
            from analyze import code_overlay_layout
            try:
                code_overlay_layout(mod_bytes)
            except ValueError as exc:
                raise IsoInspectionError(
                    f"code overlay has a malformed header: {name}: {exc}",
                    boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
                ) from exc
            modules.append({
                "name": name,
                "required": True,
                "role": "guest-prx",
                "placement": "runtime",
                "guest_path": guest_path,
            })
            continue
        interface = read_guest_module_interface(name, mod_bytes)
        if interface.callable_nids and runtime_serves_module(interface, runtime_registered_nids()):
            continue
        if interface.requires_kernel:
            kernel_modules.append((name, interface))
            continue
        module_type, module_low, module_high = _elf32_load_span(module_path)
        if module_type == 2 and module_low < main_end and main_start < module_high:
            # An executable on the main image's base is a separate program, not a
            # module the title places beside main at boot: deferred, not declared.
            deferred.append({
                "name": name,
                "reason": "separate executable on the main image base, not pre-placed",
            })
            continue
        placed_exports.update(interface.callable_nids)
        if module_type == 2:
            if (
                module_low < title_manifest.GUEST_MODULE_RAM_LO
                or module_high > PSP_CONVENTIONAL_USER_MEMORY_TOP
            ):
                raise IsoInspectionError(
                    f"guest module has fixed load address 0x{module_low:08x} that collides "
                    f"with layout: {name}",
                    boundary_code="GUEST_MODULE_LOAD_BINDING_REQUIRED",
                )
        elif module_type not in (3, 0xFFA0):
            raise IsoInspectionError(
                f"guest module is not a supported ELF/PRX: {name}",
                boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
            )
        modules.append({
            "name": name,
            "required": True,
            "role": "guest-prx",
            "placement": "runtime",
            "guest_path": guest_path,
        })

    if any(interface.callable_nids for _name, interface in kernel_modules):
        main_imports = _main_executable_imported_nids(main_elf)
        for name, interface in sorted(kernel_modules, key=lambda item: item[0].casefold()):
            missing = sorted(
                (interface.callable_nids & main_imports) - runtime_registered_nids() - placed_exports
            )
            if missing:
                nid_list = ", ".join(f"0x{nid:08x}" for nid in missing)
                raise IsoInspectionError(
                    f"guest module requires PSP kernel mode: {name}: the main executable "
                    f"imports {len(missing)} of its functions that neither the runtime nor "
                    f"a translated module provides (NIDs {nid_list})",
                    boundary_code="GUEST_MODULE_FORMAT_UNSUPPORTED",
                )

    return (
        sorted(modules, key=lambda module: module["name"].casefold()),
        sorted(deferred, key=lambda item: item["name"].casefold()),
    )


_IDENTITY_KEYS = frozenset({"DISC_ID", "TITLE", "DISC_VERSION"})


def parse_param_sfo(data: bytes) -> Dict[str, str]:
    """Parse a bounded PSP PARAM.SFO structure."""
    if len(data) < 20 or data[:8] != SFO_MAGIC:
        return {}

    try:
        key_table_start, data_table_start, entry_count = struct.unpack_from(
            "<III", data, 8
        )
    except struct.error as exc:
        raise IsoInspectionError("truncated PARAM.SFO header") from exc

    table_end = 20 + entry_count * 16
    if (
        entry_count > 256
        or key_table_start < table_end
        or key_table_start > data_table_start
        or data_table_start > len(data)
    ):
        raise IsoInspectionError("PARAM.SFO table bounds are invalid")

    result: Dict[str, str] = {}
    for index in range(entry_count):
        entry_offset = 20 + index * 16
        try:
            key_offset, param_fmt, data_len, max_len, data_offset = struct.unpack_from(
                "<HHIII", data, entry_offset
            )
        except struct.error as exc:
            raise IsoInspectionError("truncated PARAM.SFO entry table") from exc

        if key_offset > data_table_start - key_table_start:
            raise IsoInspectionError("PARAM.SFO key offset is outside the key table")
        key_start = key_table_start + key_offset
        key_end = data.find(b"\0", key_start, data_table_start)
        if key_end < 0:
            raise IsoInspectionError("PARAM.SFO key is not NUL-terminated")
        try:
            key_name = data[key_start:key_end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise IsoInspectionError("PARAM.SFO key is not ASCII") from exc

        if data_offset > len(data) - data_table_start:
            raise IsoInspectionError("PARAM.SFO data offset is outside the data table")
        data_start = data_table_start + data_offset
        if data_len > len(data) - data_start:
            raise IsoInspectionError("PARAM.SFO data length exceeds the buffer")
        if max_len < data_len:
            raise IsoInspectionError("PARAM.SFO data length exceeds max length")
        raw_value = data[data_start : data_start + data_len]

        if key_name in _IDENTITY_KEYS and param_fmt not in (
            SFO_FMT_UTF8,
            SFO_FMT_UTF8_SPECIAL,
        ):
            raise IsoInspectionError(
                f"SFO key '{key_name}' declares parameter format 0x{param_fmt:04x}, "
                "which is not a UTF-8 string"
            )

        if param_fmt == SFO_FMT_UTF8:
            value_bytes = raw_value.rstrip(b"\0")
        elif param_fmt == SFO_FMT_UTF8_SPECIAL:
            value_bytes = raw_value
        else:
            value_bytes = b""

        if param_fmt in (SFO_FMT_UTF8, SFO_FMT_UTF8_SPECIAL):
            try:
                value = value_bytes.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise IsoInspectionError(f"SFO key '{key_name}' is not valid UTF-8") from exc
        elif param_fmt == SFO_FMT_UINT32:
            if data_len < 4:
                raise IsoInspectionError(f"SFO key '{key_name}' has a short integer value")
            value = str(struct.unpack_from("<I", raw_value, 0)[0])
        else:
            value = ""

        if key_name in result:
            if result[key_name] != value:
                raise IsoInspectionError(
                    f"Conflicting duplicate SFO key '{key_name}' rejected as ambiguous: "
                    f"'{result[key_name]}' vs '{value}'"
                )
            continue
        result[key_name] = value

    return result


def _canonical_disc_id(value: str) -> str:
    normalized = "".join(
        char for char in value.strip().upper() if char not in "-_ "
    )
    if title_manifest.DISC_ID_RE.fullmatch(normalized) is None:
        raise IsoInspectionError(
            "PSP_GAME/PARAM.SFO does not contain a valid 9-character DISC_ID"
        )
    return normalized


def _pbp_display_title(title: str) -> str:
    """Show a package title the way the native reader would.

    A C string stops at the first NUL, and ASCII control characters are
    replaced with '?' so the sentence stays one plain single line.
    """
    visible = title.split("\0", 1)[0]
    return "".join(
        char if ord(char) >= 0x20 and ord(char) != 0x7F else "?" for char in visible
    )


def _pbp_package_sentence(disc_id: str, title: str) -> str:
    if title:
        return f"{PBP_UNSUPPORTED_SENTENCE} Title: {title} (ID: {disc_id})"
    return f"{PBP_UNSUPPORTED_SENTENCE} ID: {disc_id}"


def _pbp_refusal(message: str, boundary_code: str) -> IsoInspectionError:
    """Build the identify refusal for one code from PBP_BOUNDARY_CODES."""
    if boundary_code not in PBP_BOUNDARY_CODES:
        raise RuntimeError(f"unregistered PBP boundary code: {boundary_code}")
    return IsoInspectionError(message, boundary_code=boundary_code)


def _reject_pbp_package(stream, size_bytes: int, header: bytes) -> None:
    """Refuse a file already identified as a PBP package at a named boundary.

    Always raises ``IsoInspectionError``: the ``\\0PBP`` magic was seen at
    offset 0, so any ISO9660 reading of this file is a misread rather than a
    format this function can fall back to. Only the PARAM.SFO section is
    parsed, with the shared parser, so the sentence can name the title and
    disc ID the package carries. No package section is extracted or executed.
    """
    if len(header) < PBP_HEADER_SIZE:
        raise _pbp_refusal(PBP_HEADER_TRUNCATED_SENTENCE, PBP_HEADER_TRUNCATED)
    offsets = struct.unpack_from("<8I", header, 8)
    ascending = all(
        offsets[index] <= offsets[index + 1]
        for index in range(PBP_SECTION_COUNT - 1)
    )
    if (
        offsets[0] < PBP_HEADER_SIZE
        or not ascending
        or offsets[PBP_SECTION_COUNT - 1] > size_bytes
    ):
        raise _pbp_refusal(PBP_OFFSETS_INVALID_SENTENCE, PBP_OFFSETS_INVALID)

    sfo_start = offsets[PBP_PARAM_SFO_SECTION]
    sfo_size = offsets[PBP_PARAM_SFO_SECTION + 1] - sfo_start
    if sfo_size < 20 or sfo_size > MAX_SFO_BYTES:
        raise _pbp_refusal(PBP_SFO_INVALID_SENTENCE, PBP_SFO_INVALID)
    stream.seek(sfo_start)
    raw_sfo = stream.read(sfo_size)
    try:
        if len(raw_sfo) != sfo_size:
            raise IsoInspectionError("PARAM.SFO section is truncated")
        sfo = parse_param_sfo(raw_sfo)
        disc_id = _canonical_disc_id(sfo.get("DISC_ID", ""))
        title = sfo.get("TITLE", "")
        if len(title.encode("utf-8")) >= PBP_TITLE_MAX_BYTES:
            raise IsoInspectionError("PARAM.SFO TITLE is too long to display")
    except IsoInspectionError as exc:
        raise _pbp_refusal(PBP_SFO_INVALID_SENTENCE, PBP_SFO_INVALID) from exc
    raise _pbp_refusal(
        _pbp_package_sentence(disc_id, _pbp_display_title(title)),
        PBP_PACKAGE_UNSUPPORTED,
    )


def inspect_iso(
    iso_path: Path | str,
    registry: Optional[TitleRegistry] = None,
    *,
    user_data_root: Path | str | None = None,
) -> IsoMetadata:
    """Inspect a PSP ISO image and match it against supported title profiles."""
    path = Path(iso_path)
    if not path.is_file():
        raise IsoInspectionError(f"ISO file does not exist: {path}")

    # A compressed disc image (.cso, .zso, .chd) is expanded once, by the
    # player's own C code through tools/nk_core/disc_expand_tool.py, into the
    # per-user data root, and the plain .iso it becomes is what is inspected.
    source_format = disc_expand_tool.compressed_format(path)
    if source_format is not None:
        if user_data_root is not None:
            data_root = Path(user_data_root)
        else:
            from .prereq_fetcher import default_data_root

            data_root = default_data_root()
        try:
            expanded = disc_expand_tool.ensure_plain_iso(path, data_root)
        except disc_expand_tool.DiscImageRefused as exc:
            raise IsoInspectionError(str(exc), boundary_code=exc.code) from exc
        except (RuntimeError, OSError) as exc:
            raise IsoInspectionError(
                f"The compressed disc image could not be expanded: {exc}"
            ) from exc
        info = inspect_iso(expanded, registry, user_data_root=user_data_root)
        info.container_metadata["source_format"] = source_format
        return info

    size_bytes = path.stat().st_size

    # A PlayStation Store package carries "\0PBP" at offset 0 and is not a
    # disc image whatever its name or size says, so it is identified before
    # any ISO9660 reading rather than reported as a broken disc.
    with path.open("rb") as probe:
        header = probe.read(PBP_HEADER_SIZE)
        if header[: len(PBP_MAGIC)] == PBP_MAGIC:
            _reject_pbp_package(probe, size_bytes, header)

    if size_bytes < 1024 * 1024:  # Under 1 MiB is not a valid PSP UMD image
        raise IsoInspectionError(f"File is too small to be a valid PSP ISO: {size_bytes} bytes")

    reg = registry or get_default_registry()

    with open(path, "rb") as f:
        f.seek(PVD_SECTOR * SECTOR_SIZE)
        pvd_data = f.read(SECTOR_SIZE)
        if len(pvd_data) < SECTOR_SIZE or pvd_data[0:7] != ISO_MAGIC:
            raise IsoInspectionError("Not a valid ISO9660 image (missing PVD descriptor)")

        volume_id = pvd_data[40:72].decode("latin-1", errors="replace").rstrip("\x00 ")
        sfo_entry = _lookup_iso_file(f, size_bytes, ("PSP_GAME", "PARAM.SFO"))

        if sfo_entry is None:
            disc_id = "UNKNOWN"
            title = "Unknown PSP Title"
            version = "1.00"
            identity_structured = False
            sfo_facts: dict[str, str] = {}
        else:
            sfo_lba, sfo_size = sfo_entry
            if sfo_size < 20 or sfo_size > MAX_SFO_BYTES:
                raise IsoInspectionError("PSP_GAME/PARAM.SFO has an unsupported size")
            raw_sfo = _read_iso_extent(f, size_bytes, sfo_lba, sfo_size, 0, sfo_size)
            sfo_dict = parse_param_sfo(raw_sfo)
            disc_id = _canonical_disc_id(sfo_dict.get("DISC_ID", ""))
            title = sfo_dict.get("TITLE", "")
            version = sfo_dict.get("DISC_VERSION", "1.00")
            sfo_facts = {
                key: sfo_dict[key]
                for key in ("DISC_ID", "TITLE", "DISC_VERSION", "APP_VER", "PSP_SYSTEM_VER", "CATEGORY")
                if key in sfo_dict
            }
            identity_structured = True
            if not title:
                title = f"PSP Title ({disc_id})"

        region = "UNKNOWN"
        upper_disc = disc_id.upper()
        if upper_disc.startswith("UCUS") or upper_disc.startswith("ULUS"):
            region = "NA"
        elif upper_disc.startswith("UCES") or upper_disc.startswith("ULES"):
            region = "EU"
        elif upper_disc.startswith("UCJS") or upper_disc.startswith("ULJS"):
            region = "JP"
        elif upper_disc.startswith("UCAS") or upper_disc.startswith("ULAS"):
            region = "ASIA"

        matched = reg.lookup_by_disc_id(disc_id) if identity_structured else None
        qualification_error = ""
        if matched is not None and matched.require_local_compatibility_record:
            from . import package_cache

            recorded = (
                package_cache.read_local_title_input_identity(user_data_root, disc_id)
                if user_data_root is not None else None
            )
            current_region = region if region in {"JP", "NA", "EU", "KR", "ASIA", "OTHER"} else "OTHER"
            if recorded is None:
                qualification_error = (
                    f"Unqualified revision (SFO DISC_VERSION {version}) for manifest/profile "
                    f"'{matched.id}': an explicit local compatibility record is required. "
                    "Register it with --register-local-compatibility-record after reviewing the local inputs."
                )
            else:
                prior = recorded["disc"]
                if (recorded["manifest"]["id"] != matched.id or
                    prior["id"] != disc_id or prior["region"] != current_region or
                    prior["disc_version"] != version or recorded["param_sfo"] != sfo_facts):
                    if prior["disc_version"] != version:
                        changed = "SFO revision changed"
                    elif recorded["manifest"]["id"] != matched.id:
                        changed = "manifest/profile changed"
                    elif prior["id"] != disc_id or prior["region"] != current_region:
                        changed = "DISC_ID or region changed"
                    else:
                        changed = "PARAM.SFO facts changed"
                    qualification_error = (
                        f"Unqualified revision (SFO DISC_VERSION {version}): {changed}; "
                        "an explicit local compatibility record is required. "
                        "Register it with --register-local-compatibility-record after reviewing the local inputs."
                    )

        return IsoMetadata(
            disc_id=disc_id,
            title=title,
            version=version,
            region=region,
            volume_id=volume_id,
            size_bytes=size_bytes,
            matched_profile=matched,
            param_sfo_facts=sfo_facts,
            container_metadata={
                "format": "iso9660",
                "volume_id": volume_id,
                "size_bytes": size_bytes,
                "pvd_sector": PVD_SECTOR,
                "sector_size": SECTOR_SIZE,
            },
            qualification_error=qualification_error,
        )


def write_experimental_profile(
    iso_path: Path | str,
    user_data_dir: Path | str,
    *,
    metadata: Optional[IsoMetadata] = None,
) -> Path:
    """Write a validated generic profile and local executable identity for an unknown PSP disc.

    The canonical manifest is validated by ``tools/title_manifest.py`` and
    contains no title bindings. Executable hashes and the selected ISO path are
    stored only below the caller's per-user data directory.
    """
    path = Path(iso_path)
    info = metadata or inspect_iso(path)
    if info.matched_profile is not None:
        raise IsoInspectionError("catalogued titles do not need an experimental profile")

    user_root = Path(user_data_dir).expanduser()
    try:
        user_root = user_root.resolve(strict=False)
        repository_root = Path(__file__).resolve().parents[2]
        if user_root == repository_root or repository_root in user_root.parents:
            raise IsoInspectionError("experimental profiles must be stored outside the repository")
    except OSError as exc:
        raise IsoInspectionError("user data directory could not be resolved") from exc

    report = inspect_compatibility_preflight(
        path, metadata=info, runtime_root=user_root
    )
    disc_check = next(check for check in report["checks"] if check["code"] == "DISC_SFO")
    if disc_check["status"] != "OK":
        raise IsoInspectionError("experimental import requires PARAM.SFO reached through the disc directory tree")
    if not title_manifest.DISC_ID_RE.fullmatch(info.disc_id):
        raise IsoInspectionError("PARAM.SFO does not contain a valid PSP disc ID")

    disc_id = info.disc_id.upper()
    profile_id = f"experimental-{disc_id.lower()}"
    region = info.region if info.region in {"JP", "NA", "EU", "KR", "ASIA", "OTHER"} else "OTHER"
    display_name = info.title.strip() if info.title and info.title.strip() else f"PSP Title ({disc_id})"
    if any(ord(char) < 0x20 for char in display_name):
        display_name = f"PSP Title ({disc_id})"
    manifest = title_manifest.validate_manifest({
        "schema_version": 1,
        "id": profile_id,
        "game_name": profile_id,
        "display_name": display_name,
        "kind": "retail",
        "disc": {
            "id": disc_id,
            "region": region,
            "revision_policy": "exact-disc-id",
        },
        "executable": {
            "base": 0,
            "entry": 0,
            "bss_metadata_source": "none",
            "extra_executable_spans": [],
        },
        "modules": [],
        # A generic title reads its disc through PSP_ISO. The data root is the
        # extracted-archive folder only a reviewed title manifest declares, so
        # declaring one here made the player refuse every uncatalogued launch
        # for a missing folder nothing creates.
        "filesystem": {
            "data_root": "",
            "memory_stick_root": "savedata",
            "device_prefixes": ["disc0:", "ms0:"],
        },
        "hle_profile": "generic",
        "codegen_profile": "none",
        "feature_requirements": [],
        "verification_profile": "experimental-unverified",
    })

    selected = report["selected_executable"]
    selected_path = ""
    executable_sha256: str | None = None
    elf_sha256: str | None = None
    executable_entry = 0
    executable_load_address: int | None = None
    executable_entry_rebase = 0
    executable_load_evidence: str | None = None
    if selected is not None:
        uses_decrypted_eboot = selected == "EBOOT.elf"
        selected_path = (
            "PSP_GAME/SYSDIR/EBOOT.BIN"
            if uses_decrypted_eboot
            else f"PSP_GAME/SYSDIR/{selected}"
        )
        decrypted_path: Path | None = None
        if uses_decrypted_eboot:
            decrypted_value = report.get("decrypted_executable")
            if not isinstance(decrypted_value, str):
                raise IsoInspectionError("selected decrypted EBOOT.elf is unavailable")
            decrypted_path = Path(decrypted_value)
            if _classify_decrypted_elf_file(decrypted_path) != "PLAIN_MIPS_ELF32":
                raise IsoInspectionError("selected decrypted EBOOT.elf is not a usable MIPS ELF32")
            file_size = decrypted_path.stat().st_size
            if file_size <= 0 or file_size > MAX_EXECUTABLE_BYTES:
                raise IsoInspectionError("selected decrypted EBOOT.elf exceeds the supported size bound")
        else:
            file_size = path.stat().st_size
        digest = hashlib.sha256()
        with (decrypted_path if uses_decrypted_eboot else path).open("rb") as stream:
            if uses_decrypted_eboot:
                lba = 0
                size = file_size
                header = stream.read(min(size, 52))
            else:
                extent = _lookup_iso_file(
                    stream, file_size, ("PSP_GAME", "SYSDIR", selected)
                )
                if extent is None:
                    raise IsoInspectionError("selected executable disappeared from the ISO directory tree")
                lba, size = extent
                header = _read_iso_extent(stream, file_size, lba, size, 0, min(size, 52))
            if size <= 0 or size > MAX_EXECUTABLE_BYTES:
                raise IsoInspectionError("selected executable is outside the supported hashing bound")
            if len(header) < 52 or header[:7] != b"\x7fELF\x01\x01\x01":
                raise IsoInspectionError("selected executable has no supported little-endian ELF32 header")
            e_type, machine, version = struct.unpack_from("<HHI", header, 16)
            if machine != 8 or version != 1:
                raise IsoInspectionError("selected executable is not a supported MIPS ELF32 image")
            if e_type not in (2, 3, 0xFFA0):
                raise IsoInspectionError(
                    "experimental import needs a user-supplied load binding for unsupported relocatable ELF input"
                )
            executable_entry = struct.unpack_from("<I", header, 24)[0]
            if e_type in (3, 0xFFA0):
                executable_load_address = PSP_DEFAULT_MAIN_LOAD_ADDRESS
                executable_entry_rebase = PSP_DEFAULT_MAIN_LOAD_ADDRESS
                executable_load_evidence = "documented-psp-default"
            elif e_type == 2:
                def read_executable(offset: int, count: int) -> bytes:
                    if uses_decrypted_eboot:
                        stream.seek(offset)
                        return stream.read(count)
                    return _read_iso_extent(
                        stream, file_size, lba, size, offset, count
                    )

                _elf_type, load_start, load_end = _elf32_load_span_from_reader(
                    read_executable, size
                )
                if load_start == 0:
                    # Unlike ET_REL/PRX, this format has no generic default
                    # rebasing rule for a zero-address main image.
                    raise IsoInspectionError(
                        "This ET_EXEC has no guest RAM load address. Generic rebinding "
                        "for this executable type is not supported yet.",
                        boundary_code="EXECUTABLE_LOAD_BINDING_REQUIRED",
                    )
                if (load_start < title_manifest.GUEST_MODULE_RAM_LO or
                        load_end > PSP_CONVENTIONAL_USER_MEMORY_TOP):
                    raise IsoInspectionError(
                            "This fixed-address executable loads outside guest user RAM. "
                            "Building it there is not supported yet.",
                        boundary_code="FIXED_ADDRESS_EXECUTABLE_OUTSIDE_GUEST_RAM",
                    )
                executable_load_address = load_start
            offset = 0
            while offset < size:
                count = min(64 * 1024, size - offset)
                if uses_decrypted_eboot:
                    stream.seek(offset)
                    chunk = stream.read(count)
                else:
                    chunk = _read_iso_extent(stream, file_size, lba, size, offset, count)
                if len(chunk) != count:
                    raise IsoInspectionError("selected executable could not be read completely")
                digest.update(chunk)
                offset += count
        executable_sha256 = digest.hexdigest()
        elf_sha256 = executable_sha256
    if executable_entry_rebase:
        if executable_entry > 0xFFFFFFFF - executable_entry_rebase:
            raise IsoInspectionError("relocatable executable entry exceeds the guest address space")
        executable_entry += executable_entry_rebase
    manifest["executable"]["entry"] = executable_entry
    if executable_load_address is not None:
        manifest["executable"]["base"] = executable_load_address
        if executable_load_evidence is not None:
            manifest["executable"].update({
                "load_address": executable_load_address,
                "load_address_evidence": executable_load_evidence,
            })
    manifest = title_manifest.validate_manifest(manifest)

    profile = {
        "schema_version": EXPERIMENTAL_PROFILE_SCHEMA_VERSION,
        "manifest": manifest,
        "input_identity": {
            "disc_id": disc_id,
            "selected_executable": selected_path,
            "executable_sha256": executable_sha256,
            "elf_sha256": elf_sha256,
        },
    }
    profile_dir = user_root / "experimental" / disc_id
    profile_dir.mkdir(parents=True, exist_ok=True)
    resolved_dir = profile_dir.resolve(strict=True)
    if user_root != resolved_dir and user_root not in resolved_dir.parents:
        raise IsoInspectionError("experimental profile path escaped the user data directory")
    profile_path = resolved_dir / "profile.json"
    temporary_path = resolved_dir / "profile.json.tmp"
    temporary_path.write_text(
        json.dumps(profile, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary_path, profile_path)
    return profile_path


def _iso_name_without_version(name: str | bytes) -> str | bytes:
    """Strip the ISO 9660 ';version' suffix: a trailing ';' and one or more ASCII digits.

    Any other ';' is part of the name, which Windows accepts. Mirrors
    nk_iso_name_without_version in src/core/nk_iso.c, which must agree with it.
    """
    semicolon = ";" if isinstance(name, str) else b";"
    digits = "0123456789" if isinstance(name, str) else b"0123456789"
    stripped = name.rstrip(digits)
    if len(stripped) < len(name) and stripped.endswith(semicolon):
        return stripped[:-1]
    return name


def _extent_from_record(record: bytes, file_size: int) -> tuple[int, int, bool]:
    if len(record) < 34 or record[0] < 34 or record[0] > len(record):
        raise IsoInspectionError("malformed ISO directory record")
    name_size = record[32]
    if 33 + name_size > record[0]:
        raise IsoInspectionError("truncated ISO directory identifier")
    lba_le = int.from_bytes(record[2:6], "little")
    lba_be = int.from_bytes(record[6:10], "big")
    size_le = int.from_bytes(record[10:14], "little")
    size_be = int.from_bytes(record[14:18], "big")
    if lba_le != lba_be or size_le != size_be:
        raise IsoInspectionError("ISO directory extent has conflicting endian copies")
    offset = lba_le * SECTOR_SIZE
    if offset > file_size or size_le > file_size - offset:
        raise IsoInspectionError("ISO directory extent exceeds the image bounds")
    return lba_le, size_le, bool(record[25] & 0x02)


def _read_iso_extent(
    stream, file_size: int, lba: int, size: int, offset: int, count: int
) -> bytes:
    base = lba * SECTOR_SIZE
    if base > file_size or size > file_size - base or offset > size or count > size - offset:
        raise IsoInspectionError("ISO file span exceeds the image bounds")
    stream.seek(base + offset)
    data = stream.read(count)
    if len(data) != count:
        raise IsoInspectionError("ISO file span is truncated")
    return data


def _read_iso_directory_entries(
    stream, file_size: int, lba: int, size: int
) -> list[IsoDirectoryEntry]:
    if size <= 0 or size > MAX_DIRECTORY_BYTES:
        raise IsoInspectionError("ISO directory is empty or exceeds its supported bound")
    directory = _read_iso_extent(stream, file_size, lba, size, 0, size)
    entries: list[IsoDirectoryEntry] = []
    offset = 0
    while offset < len(directory):
        record_size = directory[offset]
        if record_size == 0:
            offset = ((offset // SECTOR_SIZE) + 1) * SECTOR_SIZE
            continue
        if record_size < 34 or record_size > len(directory) - offset or \
                (offset % SECTOR_SIZE) + record_size > SECTOR_SIZE:
            raise IsoInspectionError("malformed ISO directory record bounds")
        record = directory[offset : offset + record_size]
        name_size = record[32]
        if name_size == 0 or 33 + name_size > record_size:
            raise IsoInspectionError("malformed ISO directory identifier")
        raw_name = record[33 : 33 + name_size]
        offset += record_size
        if raw_name in (b"\0", b"\1"):
            continue
        # The PSP's file system takes identifier bytes as they are, and discs carry
        # Shift-JIS names. A byte outside ASCII is kept as a lone surrogate
        # (surrogateescape), so the name round-trips to its disc bytes, folds only in
        # ASCII, and can never pass the portable filename rule a host path needs.
        name = raw_name.decode("ascii", "surrogateescape")
        # ISO9660 file versions are metadata, not part of the guest filename.
        name = _iso_name_without_version(name)
        if not name or name in {".", ".."}:
            raise IsoInspectionError("ISO directory contains an invalid filename")
        entry_lba, entry_size, is_directory = _extent_from_record(record, file_size)
        entries.append(IsoDirectoryEntry(
            name=name,
            lba=entry_lba,
            size=entry_size,
            is_directory=is_directory,
            multi_extent=bool(record[25] & 0x80),
        ))
    return entries


def list_iso_directory(
    iso_path: Path | str, path: tuple[str, ...], *, require_final_directory: bool = False
) -> list[IsoDirectoryEntry] | None:
    """List one fixed ISO directory after validating every record and extent.

    ``None`` means the requested directory is absent or is not a directory, unless
    ``require_final_directory`` is set and the final component names a file. Caller
    supplied components are single names; traversal syntax and nested separators
    are refused.
    """
    if len(path) > 8 or any(
        not isinstance(component, str)
        or not component
        or component in {".", ".."}
        or "/" in component
        or "\\" in component
        for component in path
    ):
        raise IsoInspectionError("ISO directory path is invalid")
    image = Path(iso_path)
    try:
        file_size = image.stat().st_size
        with image.open("rb") as stream:
            stream.seek(PVD_SECTOR * SECTOR_SIZE)
            pvd = stream.read(SECTOR_SIZE)
            if len(pvd) != SECTOR_SIZE or pvd[:7] != ISO_MAGIC:
                raise IsoInspectionError("missing primary volume descriptor")
            lba, size, is_directory = _extent_from_record(pvd[156:190], file_size)
            if not is_directory or size == 0 or size > MAX_DIRECTORY_BYTES:
                raise IsoInspectionError("root directory is not a bounded directory extent")
            for index, component in enumerate(path):
                entries = _read_iso_directory_entries(stream, file_size, lba, size)
                found = next(
                    (entry for entry in entries
                     if entry.name.casefold() == component.casefold()),
                    None,
                )
                if found is None:
                    return None
                if not found.is_directory:
                    if require_final_directory and index == len(path) - 1:
                        raise IsoInspectionError(
                            "DISC_MODULE_TREE_INVALID: module root "
                            f"{'/'.join(path)} is not a directory."
                        )
                    return None
                lba, size = found.lba, found.size
            if not is_directory and not path:
                return None
            return _read_iso_directory_entries(stream, file_size, lba, size)
    except OSError as exc:
        raise IsoInspectionError("ISO directory could not be read") from exc


def _lookup_iso_file(stream, file_size: int, path: tuple[str, ...]) -> tuple[int, int] | None:
    stream.seek(PVD_SECTOR * SECTOR_SIZE)
    pvd = stream.read(SECTOR_SIZE)
    if len(pvd) != SECTOR_SIZE or pvd[:7] != ISO_MAGIC:
        raise IsoInspectionError("missing primary volume descriptor")
    lba, size, is_dir = _extent_from_record(pvd[156:190], file_size)
    if not is_dir or size == 0 or size > MAX_DIRECTORY_BYTES:
        raise IsoInspectionError("root directory is not a bounded directory extent")

    for index, component in enumerate(path):
        if not is_dir or size == 0 or size > MAX_DIRECTORY_BYTES:
            raise IsoInspectionError("ISO path crosses a non-directory or oversized extent")
        directory = _read_iso_extent(stream, file_size, lba, size, 0, size)
        wanted = component.upper().encode("ascii")
        found: tuple[int, int, bool] | None = None
        offset = 0
        while offset < len(directory):
            record_size = directory[offset]
            if record_size == 0:
                offset = ((offset // SECTOR_SIZE) + 1) * SECTOR_SIZE
                continue
            if record_size < 34 or record_size > len(directory) - offset or \
                    (offset % SECTOR_SIZE) + record_size > SECTOR_SIZE:
                raise IsoInspectionError("malformed ISO directory record bounds")
            record = directory[offset : offset + record_size]
            name_size = record[32]
            if name_size == 0 or 33 + name_size > record_size:
                raise IsoInspectionError("malformed ISO directory identifier")
            name = record[33 : 33 + name_size]
            if name not in (b"\0", b"\1") and _iso_name_without_version(name).upper() == wanted:
                found = _extent_from_record(record, file_size)
                break
            offset += record_size
        if found is None:
            return None
        lba, size, is_dir = found
        if index < len(path) - 1 and not is_dir:
            return None
    return (lba, size) if not is_dir else None


def _image_read(read, offset: int, count: int) -> bytes | None:
    """Bytes of one image range, or None when the range is not fully readable."""
    try:
        data = read(offset, count)
    except (IsoInspectionError, OSError):
        return None
    if data is None or len(data) != count:
        return None
    return data


def _elf32_mips_layout_violation(read, image_size: int, *, module: bool) -> str | None:
    """The PSP ELF32/MIPS image layout rule. Returns None when usable, else the name
    of the first rule the image breaks.

    Python mirror of nk_elf32_mips_layout_violation in src/core/nk_iso.c; the two
    must accept and refuse the same inputs (tools/test_iso_parity.py checks it). The
    rule copies segment bytes from their file offsets, so p_offset and p_vaddr need
    not be congruent modulo p_align. Every bounds check stays, and loadable segments
    must not overlap. ``read(offset, count)`` returns the image bytes at that range.
    ``module`` selects the guest-module entry rule: a PSP PRX (e_type 0xFFA0) needs a
    code segment, not an e_entry in an executable segment.
    """
    if image_size < 52:
        return "header-truncated"
    header = _image_read(read, 0, 52)
    if header is None:
        return "image-read-failed"
    if header[:4] != b"\x7fELF" or header[4:7] != b"\x01\x01\x01":
        return "header-magic"
    e_type, machine, version = struct.unpack_from("<HHI", header, 16)
    entry, phoff, shoff = struct.unpack_from("<III", header, 24)
    ehsize, phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHHH", header, 40)
    # The analyzer takes relocatable (1), executable (2), shared (3) and PSP PRX (0xFFA0).
    if e_type not in (1, 2, 3, 0xFFA0) or machine != 8 or version != 1 or ehsize != 52:
        return "header-fields"
    if phentsize != 32 or not 1 <= phnum <= 128 or phoff < ehsize:
        return "program-table-fields"
    if phoff + phentsize * phnum > image_size:
        return "program-table-bounds"
    if shnum:
        if shentsize != 40 or shoff < ehsize or shoff + shentsize * shnum > image_size:
            return "section-table-bounds"
    elif shoff:
        return "section-table-bounds"
    table = _image_read(read, phoff, phentsize * phnum)
    if table is None:
        return "image-read-failed"

    have_load = False
    entry_executable = False
    code_segment = False
    spans: list[tuple[int, int]] = []
    for index in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags, p_align = (
            struct.unpack_from("<8I", table, index * phentsize)
        )
        if p_offset + p_filesz > image_size:
            return "segment-file-range"
        if p_type != 1:
            continue
        if p_memsz < p_filesz:
            return "segment-memory-below-file"
        memory_end = p_vaddr + p_memsz
        if memory_end > 0x100000000:
            return "segment-memory-range"
        if p_align > 1 and p_align & (p_align - 1):
            return "segment-alignment"
        spans.append((p_vaddr, memory_end))
        have_load = True
        executable = bool(p_flags & 1)
        if executable and p_vaddr <= entry < memory_end:
            entry_executable = True
        if executable and p_filesz > 0:
            code_segment = True
    if not have_load:
        return "no-load-segment"
    # Sorted by (start, end), any overlap shows between neighbours.
    spans.sort()
    for (_start, end), (next_start, _next_end) in zip(spans, spans[1:], strict=False):
        if end > next_start:
            return "segment-overlap"
    if module and e_type == 0xFFA0:
        return None if code_segment else "no-code-segment"
    return None if entry_executable else "entry-not-executable"


def _elf32_mips_iso_violation(
    stream, file_size: int, lba: int, size: int, module: bool = False
) -> str | None:
    """The layout rule an ELF32/MIPS image in an ISO extent (or a plain file at lba 0)
    breaks, or None when it is usable."""
    return _elf32_mips_layout_violation(
        lambda offset, count: _read_iso_extent(stream, file_size, lba, size, offset, count),
        size,
        module=module,
    )


def _elf32_mips_usable(stream, file_size: int, lba: int, size: int, module: bool = False) -> bool:
    """Whether an ELF32/MIPS image is usable as a plain executable (``module=False``) or
    as a guest module (``module=True``)."""
    return _elf32_mips_iso_violation(stream, file_size, lba, size, module=module) is None


# ---------------------------------------------------------------------------
# Guest-module boundary (issue #308).  The disc's own PRX/ELF modules are
# resolved through the same per-title folder as the executable, one module at
# a time and fail closed per module.  This check decrypts nothing; the player's
# disc check does (#295).
# ---------------------------------------------------------------------------

MODULE_ROOTS = (
    ("PSP_GAME", "SYSDIR"),
    ("PSP_GAME", "USRDIR"),
)
# Depth is measured below SYSDIR/USRDIR: the issue's deepest observed layout,
# USRDIR/DATA/MODULE/MODULE, is depth 3. The extra level leaves room for common
# packaging variations while bounding traversal work.
MAX_MODULE_DIRECTORY_DEPTH = 4
MAX_MODULE_DIRECTORIES = 1024
# The largest surveyed folder contained 35+ PRXs; 256 exceeds the
# aggregate known folder counts without turning malformed images into an
# unbounded intake route.
MAX_MODULE_CANDIDATES = 256
_MODULE_SUFFIXES = {".prx", ".elf"}
_EXECUTABLE_FILENAMES = {"eboot.bin", "boot.bin", "eboot.old"}


def walk_disc_module_entries(
    iso_path: Path | str,
) -> Iterator[tuple[tuple[str, ...], IsoDirectoryEntry]]:
    """Yield files under SYSDIR/USRDIR with shared bounded module policy.

    The walk descends at most four directory levels below either root, visits
    at most 1024 directories, and excludes every subtree named ``KMODULE``.
    Returned paths are ISO member components; callers apply their own format
    and executable checks to the files.
    """
    directories = deque()
    visited = 0
    discovered = 0

    def read_module_directory(
        directory: tuple[str, ...], *, module_root: bool = False
    ) -> list[IsoDirectoryEntry] | None:
        try:
            entries = list_iso_directory(
                iso_path, directory, require_final_directory=module_root
            )
        except IsoInspectionError as exc:
            if str(exc).startswith("DISC_MODULE_TREE_INVALID:"):
                raise
            raise IsoInspectionError(
                "DISC_MODULE_TREE_INVALID: module directory "
                f"{'/'.join(directory)} could not be listed safely: {exc}"
            ) from exc
        if entries is None and not module_root:
            raise IsoInspectionError(
                "DISC_MODULE_TREE_INVALID: listed module directory could not "
                "be reopened safely."
            )
        return entries

    for root in MODULE_ROOTS:
        entries = read_module_directory(root, module_root=True)
        if entries is not None:
            discovered += 1
            directories.append((root, 0))
    while directories:
        directory, depth = directories.popleft()
        visited += 1
        if visited > MAX_MODULE_DIRECTORIES:
            raise IsoInspectionError(
                "DISC_MODULE_DIRECTORY_LIMIT: ISO module discovery exceeds "
                f"{MAX_MODULE_DIRECTORIES} directories; broader discovery is "
                "in the works."
            )
        entries = read_module_directory(directory)
        if entries is None:
            raise IsoInspectionError(
                "DISC_MODULE_TREE_INVALID: listed module directory could not "
                "be reopened safely."
            )
        for entry in entries:
            if entry.is_directory:
                if entry.name.casefold() == "kmodule":
                    continue
                if depth < MAX_MODULE_DIRECTORY_DEPTH:
                    discovered += 1
                    if discovered > MAX_MODULE_DIRECTORIES:
                        raise IsoInspectionError(
                            "DISC_MODULE_DIRECTORY_LIMIT: ISO module discovery "
                            f"exceeds {MAX_MODULE_DIRECTORIES} directories; "
                            "broader discovery is in the works."
                        )
                    directories.append((directory + (entry.name,), depth + 1))
                continue
            yield directory, entry


def _fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def _native_component(name: str) -> str:
    """A path component as the native lister spells it: one trailing dot dropped."""
    return name[:-1] if name.endswith(".") else name


def disc_module_qualified_name(member: Sequence[str]) -> str:
    """The path-qualified host name of the disc file at ``member``.

    ``<stem>.<8 hex digits><suffix>``: the digits are the 32-bit FNV-1a hash of the
    member path's bytes ('/'-joined, ASCII letters upper-cased, because the PSP's
    file system matches paths without regard to case), so the name stays readable,
    keeps its suffix and differs for every disc folder. Mirrored by
    player_module_qualified_name in src/player/player_state.c.
    """
    path_bytes = "/".join(_native_component(part) for part in member).encode(
        "ascii", "surrogateescape").upper()
    digest = f"{_fnv1a32(path_bytes):08x}"
    name = member[-1]
    stem, dot, suffix = name.rpartition(".")
    if not dot or not stem:
        return f"{name}.{digest}"
    return f"{stem}.{digest}.{suffix}"


def disc_module_host_names(
    walked: Sequence[tuple[tuple[str, ...], IsoDirectoryEntry]],
) -> dict[tuple[str, ...], str]:
    """The host file name of every file in a walked module tree, by member path.

    A module's identity is its disc path; the per-title folders key its host copy by
    file name. A file whose name (ASCII case-insensitively) no other file in the walked
    tree carries keeps that name, so a disc without such duplicates names every copy
    exactly as before. Every file sharing a name with another gets its path-qualified
    name (disc_module_qualified_name), so two disc folders holding one file name never meet
    in a host folder. The rule reads only the tree, never the files' contents, so the
    player's disc check, the decryption step and the import name each copy the same.
    """
    counts: dict[str, int] = {}
    for _directory, entry in walked:
        folded = _native_component(entry.name).casefold()
        counts[folded] = counts.get(folded, 0) + 1
    names: dict[tuple[str, ...], str] = {}
    for directory, entry in walked:
        member = directory + (entry.name,)
        unique = counts[_native_component(entry.name).casefold()] == 1
        names[member] = entry.name if unique else disc_module_qualified_name(member)
    return names


def _prx_container_header_supported(header: bytes) -> bool:
    """The bounded ``~PSP`` shape the container decoder can accept."""
    if len(header) < 0x64 or not 1 <= header[0x27] <= 4:
        return False
    sizes = struct.unpack_from("<4I", header, 0x54)[: header[0x27]]
    return all(0 < value <= MAX_EXECUTABLE_BYTES for value in sizes) and \
        sum(sizes) <= MAX_EXECUTABLE_BYTES


def list_disc_module_candidates(iso_path: Path | str) -> list[dict]:
    """The disc's own viable guest-module candidates.

    Bounded ``.prx``/``.elf`` files below the title's module roots, minus
    the executables, CFW patch modules, and any file the intake route refuses
    by name or extent (those keep their own named failure).  Each candidate
    records its member path and whether it is already plain or an encrypted
    container waiting on the boundary.
    """
    path = Path(iso_path)
    file_size = path.stat().st_size
    candidates: list[dict] = []
    seen_names: dict[str, str] = {}
    walked = list(walk_disc_module_entries(path))
    host_names = disc_module_host_names(walked)
    with path.open("rb") as stream:
        for directory, entry in walked:
            if Path(entry.name).suffix.casefold() not in _MODULE_SUFFIXES or \
                    entry.name.casefold() in _EXECUTABLE_FILENAMES:
                continue
            # The host name keys the module's copy in the per-title folders.
            name = host_names[directory + (entry.name,)]
            if not title_manifest.FILENAME_RE.fullmatch(name) or \
                    name.endswith(".") or \
                    name.split(".", 1)[0].upper() in title_manifest.WINDOWS_RESERVED:
                continue
            if entry.multi_extent or entry.size <= 0 or \
                    entry.size > MAX_EXECUTABLE_BYTES:
                continue
            header = _read_iso_extent(
                stream, file_size, entry.lba, entry.size, 0, min(entry.size, 0x64)
            )
            if header.startswith(b"\x7fELF"):
                if not _elf32_mips_usable(
                    stream, file_size, entry.lba, entry.size, module=True
                ):
                    continue
                blob = _read_iso_extent(
                    stream, file_size, entry.lba, entry.size, 0, entry.size
                )
                if len(blob) != entry.size or _has_cfw_or_kernel_only_imports(blob):
                    continue  # patch modules are excluded from every route
                kind = "plain"
            elif header.startswith(b"~PSP"):
                if not _prx_container_header_supported(header):
                    continue
                kind = "encrypted"
            elif header.startswith(b"~SCE"):
                kind = "encrypted"
            else:
                continue
            member_path = "/".join(directory + (entry.name,))
            folded_name = name.casefold()
            prior_path = seen_names.get(folded_name)
            if prior_path is not None:
                # Only a path-qualified name that equals another file's own name (or
                # a 32-bit hash collision) gets here: the copies would share one file.
                raise IsoInspectionError(
                    "DUPLICATE_DISC_MODULE_BASENAME: module host name "
                    f"{name!r} stands for both {prior_path} and {member_path}; "
                    "their copies would share one file in the per-title folder."
                )
            seen_names[folded_name] = member_path
            candidates.append({
                "name": name,
                "names": (name,),
                "members": (directory + (entry.name,),),
                "directory": directory,
                "kind": kind,
            })
            if len(candidates) > MAX_MODULE_CANDIDATES:
                raise IsoInspectionError(
                    "DISC_MODULE_CANDIDATE_LIMIT: ISO contains more than "
                    f"{MAX_MODULE_CANDIDATES} guest-module candidates; larger "
                    "module sets are in the works."
                )
    return candidates


def _module_failure(result: dict, reason: str, detail: str) -> dict:
    result.update(status="failed", reason=reason, detail=detail)
    return result


def check_needed_modules(
    iso_path: Path | str,
    *,
    user_data_root: Path | str,
    disc_id: str,
    modules: Sequence[dict] | None = None,
) -> dict:
    """Check that every module the title needs is available as a plain module.

    Each ``modules`` entry declares the spellings it can be supplied under
    (``names``, disc file name first) and the ISO members holding its disc copy
    (``members``); without ``modules`` the disc's own discovered candidates are
    used.  Per module:

    * a valid user-supplied plain copy in the per-title decrypted folder wins
      and is never overwritten (skipped / user-supplied);
    * an already plain disc copy is used as it is (skipped / plain);
    * an encrypted ``~PSP``/``~SCE`` container with no valid user copy fails
      closed as ENCRYPTED_MODULE (failed / encrypted): this check does not decrypt
      games and reads no key file;
    * anything else fails closed for that module alone (failed).

    Nothing is written: this only reads the disc and the per-title folder.
    """
    root = Path(user_data_root).expanduser()
    module_dir = decrypted_module_dir(root, disc_id)
    if modules is None:
        needs = list_disc_module_candidates(iso_path)
    else:
        needs = list(modules)
    path = Path(iso_path)
    file_size = path.stat().st_size
    results: list[dict] = []
    ready = 0
    with path.open("rb") as stream:
        for need in needs:
            names = tuple(dict.fromkeys(str(spelling) for spelling in need["names"] if spelling))
            disc_name = names[0] if names else "(unnamed module)"
            result = {
                "name": disc_name,
                "names": list(names),
                "status": "failed",
                "reason": "",
                "detail": "",
            }

            # 1. A valid user-supplied plain copy wins and is never overwritten.
            invalid_spelling: str | None = None
            user_plain: Path | None = None
            if module_dir is not None:
                for spelling in names:
                    candidate = module_dir / spelling
                    if candidate.is_file():
                        if _classify_decrypted_elf_file(candidate, module=True) == "PLAIN_MIPS_ELF32":
                            user_plain = candidate
                            break
                        if invalid_spelling is None:
                            invalid_spelling = spelling
            if user_plain is not None:
                result.update(status="skipped", reason="user-supplied", detail="")
                ready += 1
                results.append(result)
                continue
            if invalid_spelling is not None:
                results.append(_module_failure(
                    result, "user-supplied-invalid",
                    f"user-supplied {invalid_spelling} is not a usable plain MIPS ELF32",
                ))
                continue

            # 2. The disc copy decides whether anything is needed at all.
            found = None
            for member in need.get("members") or ():
                hit = _lookup_iso_file(stream, file_size, tuple(member))
                if hit is not None:
                    found = hit
                    break
            if found is None:
                results.append(_module_failure(
                    result, "missing", "the disc carries no copy of this module",
                ))
                continue
            lba, extent_size = found
            if extent_size <= 0 or extent_size > MAX_EXECUTABLE_BYTES:
                results.append(_module_failure(
                    result, "unsupported",
                    "the disc copy exceeds the supported module size",
                ))
                continue
            header = _read_iso_extent(
                stream, file_size, lba, extent_size, 0, min(extent_size, 0x64)
            )
            if header.startswith(b"\x7fELF"):
                if _elf32_mips_usable(stream, file_size, lba, extent_size, module=True):
                    result.update(status="skipped", reason="plain", detail="")
                    ready += 1
                else:
                    _module_failure(
                        result, "unsupported",
                        "the disc copy is not a usable plain MIPS ELF32",
                    )
                results.append(result)
                continue
            if not (header.startswith(b"~PSP") or header.startswith(b"~SCE")):
                results.append(_module_failure(
                    result, "unsupported",
                    "the disc copy is not a plain module or an encrypted container",
                ))
                continue

            # 3. An encrypted container with no plain user copy fails closed.
            #    The player's disc check decrypts it (#295); this one does not.
            if module_dir is None:
                results.append(_module_failure(
                    result, "folder-unavailable",
                    "the per-title decrypted folder is unavailable",
                ))
                continue
            results.append(_module_failure(
                result, "encrypted", encrypted_module_message(disc_name, module_dir),
            ))
    return {
        "module_dir": str(module_dir) if module_dir is not None else None,
        "ready": ready,
        "total": len(results),
        "results": results,
    }


def encrypted_executable_message(folder: Path | str) -> str:
    """The fail-closed text for an encrypted EBOOT.BIN with no EBOOT.elf in its folder.

    The command-line check decrypts nothing; the player's disc check does (#295).
    """
    return (
        f"ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and no decrypted EBOOT.elf is in "
        f"{folder}. The player decrypts it from your own disc when it checks the disc; "
        "you can also place a decrypted EBOOT.elf from your own copy there."
    )


def encrypted_module_message(name: str, folder: Path | str, *, label: str | None = None,
                             alternative: str | None = None) -> str:
    """The fail-closed text for an encrypted guest module with no plain copy in its folder."""
    spelling = f"{name} (or {alternative})" if alternative else name
    return (
        f"ENCRYPTED_MODULE: {label or name} is encrypted and no decrypted copy is in "
        f"{folder}. The player decrypts it from your own disc when it checks the disc; "
        f"you can also place a decrypted {spelling} from your own copy there."
    )


def _guest_modules_check(report: dict) -> dict:
    """The compatibility check line for the module boundary (no tool names)."""
    discovery_error = report.get("error")
    if isinstance(discovery_error, str) and discovery_error:
        return {
            "code": "GUEST_MODULES", "status": "UNSUPPORTED",
            "message": f"Guest module discovery stopped: {discovery_error}",
            "issues": [308],
        }
    ready = report["ready"]
    total = report["total"]
    module_dir = report.get("module_dir") or "the per-title decrypted folder"
    if ready == total:
        return {
            "code": "GUEST_MODULES", "status": "OK",
            "message": f"Guest modules: {ready} of {total} ready.",
            "issues": [],
        }
    failed = [result for result in report["results"] if result["status"] == "failed"]
    first = failed[0]
    more = f" ({len(failed) - 1} more not ready)" if len(failed) > 1 else ""
    label = first["name"]
    if first["reason"] == "encrypted":
        return {
            "code": "GUEST_MODULES", "status": "MISSING",
            "message": (
                f"Guest modules: {ready} of {total} ready; ENCRYPTED_MODULE: {label} "
                f"is encrypted{more}. The player decrypts it from your own disc when it "
                f"checks the disc; you can also place a decrypted {label} from your own "
                f"copy in {module_dir}."
            ),
            "issues": [308],
        }
    return {
        "code": "GUEST_MODULES", "status": "UNSUPPORTED",
        "message": (
            f"Guest modules: {ready} of {total} ready; {label} is not ready "
            f"({first['detail']}){more}. Supply decrypted modules at {module_dir}."
        ),
        "issues": [308],
    }


def decrypted_module_dir(user_data_root: Path | str, disc_id: str) -> Path | None:
    """Return the contained per-title folder for user-supplied decrypted inputs."""
    canonical_id = str(disc_id).upper()
    if not title_manifest.DISC_ID_RE.fullmatch(canonical_id):
        return None
    root = Path(user_data_root).expanduser().resolve(strict=False)
    candidate = root / "titles" / canonical_id / "decrypted"
    resolved = candidate.resolve(strict=False)
    try:
        resolved.relative_to(root)
    except ValueError:
        return None
    return resolved


def _classify_decrypted_elf_file(path: Path | str, *, module: bool = False) -> str:
    """Validate a user-supplied ELF32/MIPS analysis input and its guest spans.

    ``module=True`` applies the guest-module rule (see ``_elf32_mips_usable``).
    """
    candidate = Path(path)
    try:
        size = candidate.stat().st_size
        if size == 0:
            return "EMPTY_OR_ZERO_FILLED"
        if size > MAX_EXECUTABLE_BYTES:
            return "UNKNOWN"
        with candidate.open("rb") as stream:
            header = stream.read(min(size, 0x80))
            if header.startswith(b"\x7fELF"):
                # The original ELF is read by the static analyzer, not a host
                # ELF loader; its bounded guest spans remain required.
                usable = _elf32_mips_usable(stream, size, 0, size, module=module)
                return "PLAIN_MIPS_ELF32" if usable else "UNKNOWN"
    except (OSError, IsoInspectionError, struct.error):
        return "UNKNOWN"
    return "UNKNOWN"


def _classify_iso_executable(stream, file_size: int, path: str) -> dict[str, object]:
    entry = _lookup_iso_file(stream, file_size, ("PSP_GAME", "SYSDIR", path))
    if entry is None:
        return {"classification": "UNKNOWN", "present": False, "size_bytes": 0}
    lba, size = entry
    result: dict[str, object] = {
        "classification": "UNKNOWN", "present": True, "size_bytes": size,
    }
    if size == 0:
        result["classification"] = "EMPTY_OR_ZERO_FILLED"
        return result
    if size > MAX_EXECUTABLE_BYTES:
        return result
    header = _read_iso_extent(stream, file_size, lba, size, 0, min(size, 0x80))
    if header.startswith(b"\x7fELF"):
        if _elf32_mips_usable(stream, file_size, lba, size):
            result["classification"] = "PLAIN_MIPS_ELF32"
        return result
    if header.startswith(b"~SCE"):
        result["classification"] = "SCE_WRAPPER"
        return result
    if header.startswith(b"\0PBP"):
        result["classification"] = "PBP"
        return result
    if header.startswith(b"~PSP"):
        if len(header) < 0x64 or not 1 <= header[0x27] <= 4:
            return result
        sizes = struct.unpack_from("<4I", header, 0x54)[: header[0x27]]
        if any(value == 0 or value > MAX_EXECUTABLE_BYTES for value in sizes) or sum(sizes) > MAX_EXECUTABLE_BYTES:
            return result
        result["classification"] = "PSP_ENCRYPTED_CONTAINER"
        return result
    position = 0
    while position < size:
        count = min(8192, size - position)
        chunk = _read_iso_extent(stream, file_size, lba, size, position, count)
        if any(chunk):
            return result
        position += count
    result["classification"] = "EMPTY_OR_ZERO_FILLED"
    return result


def select_boot_executable_source(iso_path: Path | str) -> str | None:
    """Select an on-disc boot filename without resolving keys or decrypting files."""
    path = Path(iso_path)
    try:
        size_bytes = path.stat().st_size
        with path.open("rb") as stream:
            executables = {
                "EBOOT.BIN": _classify_iso_executable(stream, size_bytes, "EBOOT.BIN"),
                "BOOT.BIN": _classify_iso_executable(stream, size_bytes, "BOOT.BIN"),
            }
            eboot_entry = _lookup_iso_file(
                stream, size_bytes, ("PSP_GAME", "SYSDIR", "EBOOT.BIN")
            )
            old_eboot_entry = _lookup_iso_file(
                stream, size_bytes, ("PSP_GAME", "SYSDIR", "EBOOT.OLD")
            )
            cfw_loader_detected = False
            if (
                eboot_entry is not None
                and old_eboot_entry is not None
                and eboot_entry[1] <= MAX_CFW_EBOOT_SCAN_BYTES
                and executables["EBOOT.BIN"]["classification"] == "PLAIN_MIPS_ELF32"
            ):
                loader = _read_iso_extent(
                    stream, size_bytes, *eboot_entry, 0, eboot_entry[1]
                )
                cfw_loader_detected = (
                    len(loader) == eboot_entry[1]
                    and _has_cfw_or_kernel_only_imports(loader)
                )
    except (OSError, IsoInspectionError, struct.error):
        return None

    if cfw_loader_detected:
        return "EBOOT.OLD"
    eboot_kind = executables["EBOOT.BIN"]["classification"]
    boot_kind = executables["BOOT.BIN"]["classification"]
    if eboot_kind == "PSP_ENCRYPTED_CONTAINER" and boot_kind == "PLAIN_MIPS_ELF32":
        return "BOOT.BIN"
    return "EBOOT.BIN" if eboot_entry is not None else None


def inspect_compatibility_preflight(
    iso_path: Path | str,
    *,
    metadata: IsoMetadata,
    runtime_root: Path | str,
) -> dict[str, object]:
    """Return the player/CLI compatibility checks for a selected ISO."""
    path = Path(iso_path)
    size_bytes = path.stat().st_size
    directory_error: str | None = None
    sfo_entry: tuple[int, int] | None = None
    cfw_loader_detected = False
    eboot_entry: tuple[int, int] | None = None
    try:
        with path.open("rb") as stream:
            sfo_entry = _lookup_iso_file(stream, size_bytes, ("PSP_GAME", "PARAM.SFO"))
            sfo_parsed = False
            if sfo_entry is not None and 20 <= sfo_entry[1] <= MAX_SFO_BYTES:
                raw_sfo = _read_iso_extent(stream, size_bytes, *sfo_entry, 0, sfo_entry[1])
                try:
                    values = parse_param_sfo(raw_sfo)
                    _canonical_disc_id(values.get("DISC_ID", ""))
                    sfo_parsed = True
                except IsoInspectionError:
                    sfo_parsed = False
            executables = {
                "EBOOT.BIN": _classify_iso_executable(stream, size_bytes, "EBOOT.BIN"),
                "BOOT.BIN": _classify_iso_executable(stream, size_bytes, "BOOT.BIN"),
            }
            eboot_entry = _lookup_iso_file(
                stream, size_bytes, ("PSP_GAME", "SYSDIR", "EBOOT.BIN")
            )
            old_eboot_entry = _lookup_iso_file(
                stream, size_bytes, ("PSP_GAME", "SYSDIR", "EBOOT.OLD")
            )
            if (
                eboot_entry is not None
                and old_eboot_entry is not None
                and eboot_entry[1] <= MAX_CFW_EBOOT_SCAN_BYTES
                and executables["EBOOT.BIN"]["classification"] == "PLAIN_MIPS_ELF32"
            ):
                loader = _read_iso_extent(
                    stream, size_bytes, *eboot_entry, 0, eboot_entry[1]
                )
                cfw_loader_detected = (
                    len(loader) == eboot_entry[1]
                    and _has_cfw_or_kernel_only_imports(loader)
                )
    except (OSError, IsoInspectionError, struct.error) as exc:
        sfo_parsed = False
        directory_error = str(exc)
        cfw_loader_detected = False
        executables = {
            name: {"classification": "UNKNOWN", "present": False, "size_bytes": 0}
            for name in ("EBOOT.BIN", "BOOT.BIN")
        }

    eboot_kind = executables["EBOOT.BIN"]["classification"]
    boot_kind = executables["BOOT.BIN"]["classification"]
    selected = "EBOOT.BIN" if eboot_kind == "PLAIN_MIPS_ELF32" else None
    fallback = False
    if eboot_kind == "PSP_ENCRYPTED_CONTAINER" and boot_kind == "PLAIN_MIPS_ELF32":
        selected = "BOOT.BIN"
        fallback = True
    root = Path(runtime_root)
    module_dir = decrypted_module_dir(root, metadata.disc_id)
    decrypted_elf: Path | None = None
    decrypted_elf_kind = "MISSING"
    if module_dir is not None:
        candidate_elf = module_dir / "EBOOT.elf"
        try:
            candidate_elf.resolve(strict=False).relative_to(root.expanduser().resolve(strict=False))
        except ValueError:
            candidate_elf = None
        if candidate_elf is not None and candidate_elf.is_file():
            decrypted_elf = candidate_elf
            decrypted_elf_kind = _classify_decrypted_elf_file(candidate_elf)
    if cfw_loader_detected:
        selected = (
            "EBOOT.elf"
            if decrypted_elf is not None and decrypted_elf_kind == "PLAIN_MIPS_ELF32"
            else None
        )
    user_decryptable_kinds = {
        "PSP_ENCRYPTED_CONTAINER", "SCE_WRAPPER", "PBP",
    }
    # This check decrypts nothing and reads no key file (the player's disc check
    # decrypts, #295): an encrypted
    # executable is analyzed only through a user-supplied EBOOT.elf.
    if (
        not cfw_loader_detected
        and
        selected is None
        and eboot_kind in user_decryptable_kinds
        and decrypted_elf is not None
        and decrypted_elf_kind == "PLAIN_MIPS_ELF32"
    ):
        selected = "EBOOT.elf"
    # Guest-module readiness (issue #308): every module the disc carries is
    # resolved through the same per-title folder as the executable, one module
    # at a time and fail closed per module.
    module_report: dict[str, object] = {
        "module_dir": None, "ready": 0, "total": 0, "results": [],
    }
    if directory_error is None:
        try:
            module_report = check_needed_modules(
                path, user_data_root=root, disc_id=metadata.disc_id,
            )
        except (OSError, IsoInspectionError, struct.error) as exc:
            module_report = {
                "module_dir": None, "ready": 0, "total": 0, "results": [],
                "error": str(exc),
            }
    modules_check = (
        _guest_modules_check(module_report)
        if module_report["total"] or module_report.get("error")
        else None
    )
    if directory_error:
        disc_check = {
            "code": "DISC_SFO", "status": "UNSUPPORTED",
            "message": "Disc directories could not be read safely; PARAM.SFO and executables were not trusted.",
            "issues": [],
        }
    elif sfo_parsed:
        disc_check = {
            "code": "DISC_SFO", "status": "OK",
            "message": "Disc image is readable and PARAM.SFO was parsed.", "issues": [],
        }
    elif sfo_entry is not None:
        disc_check = {
            "code": "DISC_SFO", "status": "UNSUPPORTED",
            "message": "PARAM.SFO is present but malformed; title identity was not trusted.",
            "issues": [],
        }
    else:
        disc_check = {
            "code": "DISC_SFO", "status": "MISSING",
            "message": "Disc image is readable, but PSP_GAME/PARAM.SFO is missing or could not be parsed.",
            "issues": [],
        }

    if cfw_loader_detected and selected is None:
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": (
                "Custom-firmware-patched dump detected; EBOOT.OLD is the game executable "
                "but is still encrypted. Supply its decrypted form at "
                "titles/<DISC_ID>/decrypted/EBOOT.elf in user data. CFW dump intake is "
                "in the works."
            ),
            "issues": [308],
        }
    elif selected == "BOOT.BIN":
        executable_check = {
            "code": "EXECUTABLE", "status": "OK",
            "message": "BOOT.BIN selected for analysis because EBOOT.BIN is encrypted.",
            "issues": [],
        }
    elif selected == "EBOOT.BIN":
        executable_check = {
            "code": "EXECUTABLE", "status": "OK",
            "message": "EBOOT.BIN is a plain MIPS ELF32 and selected for analysis.",
            "issues": [],
        }
    elif selected == "EBOOT.elf" and decrypted_elf is not None:
        executable_check = {
            "code": "EXECUTABLE", "status": "OK",
            "message": "User-supplied decrypted EBOOT.elf is a valid MIPS ELF32 and selected for analysis.",
            "issues": [],
        }
    elif eboot_kind in user_decryptable_kinds and decrypted_elf is not None:
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": (
                f"Encrypted executable: {decrypted_elf} is invalid or not a usable MIPS ELF32; "
                f"replace it with a valid decrypted EBOOT.elf and required PRXs at {module_dir}."
            ),
            "issues": [],
        }
    elif eboot_kind in user_decryptable_kinds and module_dir is not None:
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": encrypted_executable_message(module_dir),
            "issues": [308],
        }
    elif eboot_kind == "PSP_ENCRYPTED_CONTAINER":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "Encrypted executable format is not supported yet; broader ISO-to-Play support is in the works.",
            "issues": [308],
        }
    elif eboot_kind == "EMPTY_OR_ZERO_FILLED":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "EBOOT.BIN is empty or zero-filled and cannot be analyzed.", "issues": [],
        }
    elif eboot_kind == "SCE_WRAPPER":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "~SCE wrapper could not be analyzed; broader ISO-to-Play support is in the works.",
            "issues": [308],
        }
    elif eboot_kind == "PBP":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "PBP executable unpacking is not supported yet; broader ISO-to-Play support is in the works.",
            "issues": [308],
        }
    else:
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "Unknown/malformed executable boundary; broader title support is in the works.",
            "issues": [308],
        }

    from .launcher import RuntimeLauncher

    package_present = RuntimeLauncher(repo_root=root).runtime_package_available(
        metadata.matched_profile
    )
    is_experimental = (
        metadata.matched_profile is None and disc_check["status"] == "OK" and
        title_manifest.DISC_ID_RE.fullmatch(metadata.disc_id.upper()) is not None
    )
    experimental_check = {
        "code": "EXPERIMENTAL",
        "status": "IN_PROGRESS",
        "message": "Experimental: this game has not been verified. Compatibility is unknown. Verification for additional titles and generic title intake are in the works.",
        "issues": [308],
    } if is_experimental else None

    if is_experimental:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "MISSING",
            "message": "Experimental title runtime package is missing; build it from the library.",
            "issues": [308],
        }
    elif metadata.matched_profile is None:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "UNSUPPORTED",
            "message": "Title profile missing; generic title support is in the works.",
            "issues": [308],
        }
    elif package_present:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "OK",
            "message": "Generated runtime executable and image are present.", "issues": [],
        }
    else:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "MISSING",
            "message": "Runtime package missing; build it from the library.",
            "issues": [308],
        }

    data_root_check: dict[str, object] | None = None
    if metadata.matched_profile is not None:
        declared_data_root = metadata.matched_profile.archive_relpath
        if not declared_data_root:
            data_root_check = {
                "code": "DATA_ROOT", "status": "OK",
                "message": "This game does not need a separate data folder.",
                "issues": [],
            }
        else:
            data_root_path = root / declared_data_root
            extracted_data_root = (
                Path(iso_path).resolve(strict=False).parent
                / "EXTRACTED" / "PSP_GAME" / "USRDIR" / declared_data_root
            )
            if data_root_path.is_dir() or extracted_data_root.is_dir():
                data_root_check = {
                    "code": "DATA_ROOT", "status": "OK",
                    "message": "This game's data folder is available.", "issues": [],
                }
            else:
                data_root_check = {
                    "code": "DATA_ROOT", "status": "MISSING",
                    "message": (
                        f"This game needs its '{declared_data_root}' data folder, but it is missing. "
                        "Add the game's data files before playing; broader ISO-to-play support "
                        "is coming later."
                    ),
                    "issues": [308],
                }

    from .fonts import inspect_font_cache, latin_index_states, slot_states

    font_status, font_message = inspect_font_cache(user_data_root=root)
    if font_status == "INVALID":
        fonts_check = {
            "code": "SYSTEM_FONTS", "status": "INVALID",
            "message": font_message, "issues": [313],
        }
    else:
        # Per slot, as the player's preflight: an imported font, else the project font, else missing.
        states = slot_states(user_data_root=root, project_root=root)
        latin_states = latin_index_states(user_data_root=root, project_root=root)
        slot_message = " ".join(
            [states["japanese"]["detail"]]
            + [state["detail"] for state in latin_states]
            + [states["korean"]["detail"]]
        )
        exact = (states["japanese"]["source"] == "user" and
                 states["korean"]["source"] == "user" and
                 all(state["source"] == "user" and state["served_index"] == index
                     for index, state in enumerate(latin_states)))
        if (states["japanese"]["source"] != "none" and
                states["korean"]["source"] != "none" and
                all(state["source"] != "none" for state in latin_states)):
            # Every font served; SYNTHETIC when one is a stand-in, not the user's own (#1300).
            fonts_check = {
                "code": "SYSTEM_FONTS", "status": "OK" if exact else "SYNTHETIC",
                "message": slot_message, "issues": [] if exact else [313],
            }
        else:
            fonts_check = {
                "code": "SYSTEM_FONTS", "status": "MISSING",
                "message": slot_message, "issues": [313],
            }
    audio_check = {
        "code": "AUDIO_OUTPUT", "status": "OK",
        "message": "Sound plays through your default audio device. "
                   "With no device, the game runs silently.",
        "issues": [],
    }
    checks = [disc_check]
    if cfw_loader_detected:
        checks.append({
            "code": "MODIFIED_DUMP_CFW_LOADER",
            "status": "IN_PROGRESS" if selected == "EBOOT.elf" else "UNSUPPORTED",
            "message": (
                "Custom-firmware-patched dump: using the original executable; EBOOT.OLD "
                "is the game executable selected through the supplied decrypted EBOOT.elf. "
                "The EBOOT.BIN loader and "
                "custom-firmware patch modules are excluded. Broader CFW dump support "
                "is in the works."
                if selected == "EBOOT.elf"
                else "Custom-firmware-patched dump detected; EBOOT.OLD is the game "
                     "executable but is still encrypted. Supply its decrypted form at "
                     "titles/<DISC_ID>/decrypted/EBOOT.elf in user data. CFW dump intake "
                     "is in the works."
            ),
            "issues": [308],
        })
    checks.append(executable_check)
    if modules_check is not None:
        checks.append(modules_check)
    checks.append(runtime_check)
    if data_root_check is not None:
        checks.append(data_root_check)
    checks.extend((fonts_check, audio_check))
    if experimental_check is not None:
        checks.insert(0, experimental_check)
    return {
        "is_experimental": is_experimental,
        "selected_executable": selected,
        "selected_executable_source": "EBOOT.OLD" if cfw_loader_detected else selected,
        "decrypted_module_dir": str(module_dir) if module_dir is not None else None,
        "module_decryption": module_report,
        "decrypted_executable": (
            str(decrypted_elf.resolve(strict=False))
            if selected == "EBOOT.elf" and decrypted_elf is not None
            else None
        ),
        "modified_dump_cfw_loader": cfw_loader_detected,
        "boot_fallback": fallback,
        "executables": executables,
        "checks": checks,
    }
