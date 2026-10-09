# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Parse a PSP PRX import table from the rebased+relocated image and map each import stub
# address to (library name, NID).
#
# The load-bearing structure is the one psp-fixup-imports (pspsdk/tools) produces:
# .sceStub.text holds one 8-byte slot per imported function and .rodata.sceNid holds
# one 4-byte NID per function, and the two arrays pair GLOBALLY by position: the k-th
# stub slot is the function whose NID is the k-th NID. psp-fixup-imports verifies that
# invariant (it aborts on any slot whose embedded NID differs from the section NID).
# The SceModuleInfo libstub table then lists one PspLibStubEntry per library naming a
# run of numFuncs consecutive positions (nidData/firstSym point at the run's first
# position). When the linker interleaves stub libraries the runs overlap and can leave
# trailing positions unclaimed; psp-fixup-imports warns ("stubs out of order... your
# binary may or may not work") and the loader patches only the covered positions.
#
# Parsing by the per-entry run alone (stub = firstSym + i*8) therefore both drops the
# unclaimed slots and double-counts overlapped ones. The pairing is the layout model
# psp_import_table.layout_import_windows, which the strict audit parser shares with
# this module: it derives the stub/NID regions, pairs every slot with its global NID,
# attributes library names by window claims, reports unclaimed and multi-claimed
# slots as findings, and fails closed on malformed or unpairable layouts.
# AuditAnalyzerParityTests in test_imports.py keeps the two parsers in step. This
# module supplies the analyzer's own reads: guest strings, the rebased and relocated
# image, the module-info record, and the variable-import boundary.
#
# The PSP loader itself only ever reads the windows: an entry patches the slot at
# firstSym + 8*i with the function named by nidData[i]. Every window therefore has a
# pairing offset firstSym - 2*nidData, and the windows of one psp-fixup-imports region
# share it. Retail executables can also carry windows whose stub run and NID run both
# lie outside the named sections (for example imports from a game-supplied module
# whose stubs the linker placed in .text and whose NIDs sit beside its library name in
# .rodata.sceResident). Such a window is a detached run paired by its own slots; runs
# with different offsets must never claim the same stub slot. Inputs without named
# sections keep the single union region, because without section bounds a detached
# run cannot be told apart from a corrupted window.
#
# Usage: imports.py <prx-elf> <base-hex> [--toml out.toml]

from dataclasses import dataclass
import json
import os
import struct
import sys

# The Windows embeddable Python runtime uses a fixed ``._pth`` search path and
# may omit the directory containing this script. Keep sibling tool imports
# available when Make launches this script from that runtime.
_TOOLS_DIRECTORY = os.path.dirname(os.path.abspath(__file__))
if _TOOLS_DIRECTORY not in sys.path:
    sys.path.insert(0, _TOOLS_DIRECTORY)

import build_profile

from analyze import Elf
from psp_import_table import UNATTRIBUTED_LIBRARY as UNATTRIBUTED_LIBRARY  # re-exported marker
from psp_import_table import (
    ImportWindow,
    layout_import_windows,
)
from psp_import_table import ImportTableError as LayoutImportError


# Guest strings are metadata, not trusted host-language source. PSP import-library names in
# practice are tiny (sceKernelLibrary, sceDisplay, etc.); keep a generous hard ceiling so a
# malformed/unmapped string cannot make the offline pipeline walk an unbounded address range.
MAX_IMPORT_LIBRARY_NAME_BYTES = 1024


class ImportTableError(ValueError):
    """A fail-closed import-table boundary with a stable public code."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code

    def format_boundary(self, subject=None):
        if subject is not None:
            return f"{self.code}: {subject}: {self}"
        return f"{self.code}: {self}"


def format_boundary(exc, subject=None):
    """Format a named ImportTableError boundary, optionally naming the module."""
    return exc.format_boundary(subject=subject)


@dataclass(frozen=True)
class VariableImportTable:
    """One library's declared variable imports (SceLibraryStubTable.vstubcount).

    entry is the PspLibStubEntry address and vstub_table its variable-stub
    table pointer (the sixth word of an entry at least six words long).
    """

    library: str
    count: int
    entry: int
    vstub_table: int


class VariableImportsUnsupported(ImportTableError):
    """A well-formed import table that declares variable imports.

    The analyzer records every declaring library; resolving each variable's
    references and providing exported-variable storage is not implemented, so
    the table stops at this named boundary instead of being partly imported.
    """

    def __init__(self, tables):
        self.tables = tuple(tables)
        total = sum(table.count for table in self.tables)
        summary = ", ".join(f"{table.library} x{table.count}" for table in self.tables)
        super().__init__(
            "ANALYZER_VARIABLE_IMPORTS_UNSUPPORTED",
            f"import table declares {total} variable imports ({summary}); "
            "variable imports are not supported yet",
        )


def _variable_import_table(elf, pos, entry_bytes, size_words, library, count, rebase):
    """Validate and record one entry's variable-import declaration.

    The variable-stub table pointer follows the function-stub pointer, so an
    entry that declares variables must be at least six words long and point at
    a word-aligned, mapped table.
    """
    if size_words < 6:
        raise ImportTableError(
            "ANALYZER_IMPORT_TABLE_INVALID",
            f"import entry at 0x{pos:08x} declares {count} variables but its "
            f"{size_words}-word entry has no variable-stub table",
        )
    if len(entry_bytes) < 24:
        raise ValueError(f"truncated import stub entry at 0x{pos:08x}")
    vstub_table = rebase(struct.unpack_from("<I", entry_bytes, 20)[0])
    if vstub_table == 0 or vstub_table % 4:
        raise ImportTableError(
            "ANALYZER_IMPORT_TABLE_INVALID",
            f"import entry at 0x{pos:08x} declares {count} variables but its "
            f"variable-stub table pointer 0x{vstub_table:08x} is null or misaligned",
        )
    head = elf.read_at_vaddr(vstub_table, 4)
    if head is None or len(head) != 4:
        raise ImportTableError(
            "ANALYZER_IMPORT_TABLE_INVALID",
            f"import entry at 0x{pos:08x}: variable-stub table 0x{vstub_table:08x} "
            "leaves mapped input",
        )
    return VariableImportTable(library, count, pos, vstub_table)




def _encode_import_library_name(raw):
    """Return a reversible source/config-safe ASCII representation of guest bytes.

    Normal PSP library names are unchanged. Every byte outside the deliberately tiny safe
    diagnostic alphabet is percent-encoded, including %, comment delimiters, quotes,
    backslashes, control bytes and non-ASCII bytes. parse_imports() is consumed directly by
    codegen.py, which historically interpolated the name into a generated C comment, so this
    encoding is a trust-boundary property rather than cosmetic display escaping.
    """
    if not isinstance(raw, (bytes, bytearray)):
        raise TypeError("raw import-library name must be bytes")
    safe = bytearray(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")
    allowed = set(safe)
    return "".join(chr(b) if b in allowed else f"%{b:02X}" for b in raw)


def _read_guest_cstr(elf, addr, *, max_bytes=MAX_IMPORT_LIBRARY_NAME_BYTES):
    """Read one mapped guest C string with deterministic byte/address bounds."""
    if not isinstance(addr, int) or addr < 0 or addr > 0xFFFFFFFF:
        raise ValueError(f"invalid guest string address: {addr!r}")
    if max_bytes <= 0:
        raise ValueError("max_bytes must be positive")

    out = bytearray()
    for offset in range(max_bytes + 1):
        cur = addr + offset
        if cur > 0xFFFFFFFF:
            raise ValueError("guest string address wraps 32-bit address space")
        ch = elf.read_at_vaddr(cur, 1)
        if ch is None or len(ch) != 1:
            raise ImportTableError(
                "ANALYZER_IMPORT_LIBRARY_NAME_UNMAPPED",
                f"guest string at 0x{addr:08x} leaves mapped input",
            )
        if ch[0] == 0:
            return _encode_import_library_name(out)
        if offset == max_bytes:
            break
        out.append(ch[0])
    raise ValueError(
        f"guest import-library name at 0x{addr:08x} exceeds "
        f"{max_bytes} bytes without a terminator"
    )


def _toml_basic_string(value):
    """Encode safe diagnostic text as a TOML-compatible basic string.

    JSON's quoted-string escape set used here (\\b/\\t/\\n/\\f/\\r, quote, backslash and
    \\uXXXX/\\UXXXXXXXX escapes with ensure_ascii=True) is valid inside a TOML basic string.
    Keeping the encoder in one place prevents even future non-guest metadata from becoming
    TOML syntax through manual interpolation.
    """
    if not isinstance(value, str):
        raise TypeError("TOML string value must be str")
    return json.dumps(value, ensure_ascii=True)


def _import_model_impl(elf):
    """Return (stubs, findings) for a PSP ELF import table.

    stubs maps every stub-slot address in the pairing regions to
    (library name, NID); slots no window claims map to the
    UNATTRIBUTED_LIBRARY marker. findings is a list of deterministic
    structural strings (unreferenced section words, detached runs, and
    unclaimed and multi-claimed positions).
    """
    stubs, findings, _variable_tables = _function_import_model(elf, allow_variables=False)
    return stubs, findings


def function_import_model(elf):
    """Return (stubs, findings, variable_tables) without refusing variable imports.

    The same layout as parse_imports. The analyzer refuses an image that declares
    variable imports; the import-stub census uses this entry point to enumerate the
    function stubs of such an image and name each one as missing, so none is silent.
    """
    return _function_import_model(elf, allow_variables=True)


def function_import_windows(elf):
    """Return (windows, variable_tables) of an import table, without pairing its slots.

    The census uses this for an image whose layout the model refuses. The loader reads
    the windows directly (slot firstSym + 8*i takes NID nidData[i]), so every function
    slot can still be named and accounted for.
    """
    windows, variable_tables, _base = _walk_import_windows(elf)
    return windows, tuple(variable_tables)


def _walk_import_windows(elf):
    """Walk the PspLibStubEntry table; return (windows, variable_tables, base)."""
    mi = elf.sec(".rodata.sceModuleInfo")
    if not mi:
        raise ValueError("no .rodata.sceModuleInfo section")
    b = elf.read_at_vaddr(mi["addr"], 52)
    if b is None or len(b) != 52:
        raise ValueError("truncated .rodata.sceModuleInfo")
    libstub, libstubend = struct.unpack("<2I", b[44:52])
    base = getattr(elf, "base", 0) or 0
    if base != 0:
        if libstub < base:
            libstub += base
            libstubend += base

    def rebase(v):
        if v and base != 0 and v < base:
            return v + base
        return v

    # Pass 1: walk the PspLibStubEntry window table (libstub..libstubend).
    windows = []  # ImportWindow(library, numFuncs, nidData, firstSym), in table order
    variable_tables = []  # one VariableImportTable per window that declares variables
    pos = libstub
    while pos < libstubend:
        e = elf.read_at_vaddr(pos, 28)
        if e is None or len(e) < 20:
            raise ValueError(f"truncated import stub entry at 0x{pos:08x}")
        name_ptr, ver, flags, size, numVars, numFuncs, nidData, firstSym = struct.unpack(
            "<IHHBBHII", e[:20])
        if size == 0:
            break
        name_ptr, nidData, firstSym = rebase(name_ptr), rebase(nidData), rebase(firstSym)
        if numFuncs > 0 and nidData == 0:
            raise ImportTableError(
                "ANALYZER_IMPORT_NID_TABLE_MISSING",
                f"import entry at 0x{pos:08x}: {numFuncs} functions but null NID table pointer",
            )
        if (numFuncs > 0 or numVars > 0) and name_ptr == 0:
            raise ImportTableError(
                "ANALYZER_IMPORT_LIBRARY_NAME_UNMAPPED",
                f"import entry at 0x{pos:08x} has imports but a null library-name pointer",
            )
        # A window that imports nothing claims no import stubs, so its library
        # name is never used by the codegen map. Some stripped retail inputs leave
        # this optional pointer stale; do not dereference it for an empty window.
        libname = (_read_guest_cstr(elf, name_ptr)
                   if name_ptr and (numFuncs or numVars) else "(null)")
        if numVars:
            variable_tables.append(
                _variable_import_table(elf, pos, e, size, libname, numVars, rebase))
        windows.append(ImportWindow(libname, numFuncs, nidData, firstSym))
        step = size * 4
        if step <= 0 or pos + step > 0xFFFFFFFF:
            raise ValueError("import stub table step wraps 32-bit guest space")
        pos += step
    return windows, variable_tables, base


def _function_import_model(elf, *, allow_variables):
    windows, variable_tables, base = _walk_import_windows(elf)
    if variable_tables and not allow_variables:
        raise VariableImportsUnsupported(variable_tables)

    # Passes 2-4: the shared layout model (psp_import_table.layout_import_windows)
    # pairs stub slots with NIDs for the audit parser and the analyzer alike.
    def sec_base(s):
        return s["addr"] + base if (base != 0 and s["addr"] < base) else s["addr"]

    st = elf.sec(".sceStub.text")
    nidsec = elf.sec(".rodata.sceNid")
    stub_section = (sec_base(st), st["size"]) if st is not None else None
    nid_section = (sec_base(nidsec), nidsec["size"]) if nidsec is not None else None

    def read_nids(address, count):
        blob = elf.read_at_vaddr(address, count * 4)
        if blob is None or len(blob) != count * 4:
            raise ValueError(f"truncated import NID region at 0x{address:08x}")
        return struct.unpack(f"<{count}I", blob)

    stubs, findings = layout_import_windows(windows, stub_section, nid_section, read_nids)
    return stubs, findings, tuple(variable_tables)


def _import_model(elf):
    """Return imports or a stable, fail-closed analyzer boundary."""
    try:
        return _import_model_impl(elf)
    except ImportTableError:
        raise
    except LayoutImportError as exc:
        raise ImportTableError(exc.code or "ANALYZER_IMPORT_TABLE_INVALID", str(exc)) from exc
    except (ValueError, RuntimeError, struct.error) as exc:
        raise ImportTableError("ANALYZER_IMPORT_TABLE_INVALID", str(exc)) from exc


def parse_imports(elf):
    """Return {stub address: (library name, NID)} for every import stub slot."""
    stubs, _findings = _import_model(elf)
    return stubs


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]

    use_env_elf = "--env-elf" in opts
    expected = 1 if use_env_elf else 2
    if len(args) < expected:
        sys.stderr.write(
            "usage: imports.py <prx-elf> <base-hex> [--toml out.toml]\n"
            "       imports.py --env-elf <base-hex> [--toml out.toml]\n"
        )
        return 2
    if use_env_elf and len(args) > 1:
        sys.stderr.write(
            "imports: --env-elf takes exactly one positional (<base-hex>); refusing to guess "
            f"which of {args!r} is the base address. Pass the ELF or --env-elf, not both.\n"
        )
        return 2

    cli_elf = None if use_env_elf else args[0]
    base_hex = args[0] if use_env_elf else args[1]
    try:
        elf_path = build_profile.resolve_path(
            "GAME_ELF", cli_value=cli_elf, use_env=use_env_elf, cli_label="<prx-elf> argument", flag="--env-elf",
        )
    except build_profile.BuildInputError as exc:
        sys.stderr.write(f"imports: {exc}\n")
        return 2

    try:
        base_val = int(base_hex, 16)
    except ValueError:
        sys.stderr.write(f"imports: invalid base address: {base_hex}\n")
        return 2

    elf = Elf(elf_path, base=base_val)
    try:
        stubs, findings = _import_model(elf)
    except ValueError as exc:
        sys.stderr.write(f"imports: {exc}\n")
        return 2

    by_lib = {}
    for addr, (lib, nid) in stubs.items():
        by_lib.setdefault(lib, []).append((addr, nid))
    print(f"imports: {len(stubs)} stubs across {len(by_lib)} libraries")
    for lib in sorted(by_lib):
        print(f"  {lib}: {len(by_lib[lib])}")
    for finding in findings:
        print(f"note: {finding}")

    out = None
    for a in argv[1:]:
        if a.startswith("--toml"):
            out = a.split("=", 1)[1] if "=" in a else argv[argv.index(a) + 1]
    if out:
        lines = ["# Import map emitted by tools/imports.py: stub address -> (library, NID).", ""]
        for addr in sorted(stubs):
            lib, nid = stubs[addr]
            lines.append("[[import]]")
            lines.append(f"stub = 0x{addr:08x}")
            lines.append(f"lib = {_toml_basic_string(lib)}")
            lines.append(f"nid = 0x{nid:08x}")
            lines.append("")
        with open(out, "w", encoding="ascii", newline="\n") as fh:
            fh.write("\n".join(lines))
        print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
