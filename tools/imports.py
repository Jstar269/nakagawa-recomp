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
# unclaimed slots and double-counts overlapped ones. This module instead:
#   * derives the full stub/NID regions (.sceStub.text/.rodata.sceNid when present,
#     else the union of the window runs), requiring stub_bytes == 2 * nid_bytes;
#   * pairs every stub slot with its global NID;
#   * treats each window as a claim of numFuncs positions, attributing the library
#     name (the last claimer wins on overlap -- the toolchain emits each library's
#     run from its first slot, so the last window to reach a position owns it);
#   * emits exactly one (stub_addr -> (library, NID)) pair per slot, using the
#     "(unattributed)" marker for slots no window claims, and reports structural
#     findings (unclaimed/ambiguously claimed slots) via the findings list;
#   * fails closed on malformed bounds, truncated records, overflow, impossible
#     counts, and windows whose NID position disagrees with their stub position.
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


# Marker for stub slots that no library window claims (interleaved stub tables). Kept
# out of the guest-name alphabet (percent-encoding turns any guest byte into %XX) so it
# can never collide with a real library name.
UNATTRIBUTED_LIBRARY = "(unattributed)"


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
    windows = []  # (library name, numFuncs, numVars, nidData, firstSym)
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
        if numFuncs > 0 and name_ptr == 0:
            raise ImportTableError(
                "ANALYZER_IMPORT_LIBRARY_NAME_UNMAPPED",
                f"import entry at 0x{pos:08x} has functions but a null library-name pointer",
            )
        # A zero-function window claims no import stubs, so its library name is
        # never used by the codegen map. Some stripped retail inputs leave this
        # optional pointer stale; do not dereference it for an empty window.
        libname = _read_guest_cstr(elf, name_ptr) if name_ptr and numFuncs else "(null)"
        windows.append((libname, numFuncs, numVars, nidData, firstSym))
        step = size * 4
        if step <= 0 or pos + step > 0xFFFFFFFF:
            raise ValueError("import stub table step wraps 32-bit guest space")
        pos += step
    if not windows:
        # A PSP module may legally import nothing: an empty declared window
        # table with no stub/NID regions is a valid zero-import module, not a
        # malformed one. Fail only when the shapes disagree.
        st_empty = True
        nid_empty = True
        st = elf.sec(".sceStub.text")
        nidsec = elf.sec(".rodata.sceNid")
        if st is not None and st["size"]:
            st_empty = False
        if nidsec is not None and nidsec["size"]:
            nid_empty = False
        if st_empty and nid_empty:
            return {}, ["module declares an empty import table"]
        raise ValueError("import stub table is empty")
    variable_windows = [w for w in windows if w[2] > 0]
    if variable_windows:
        variable_count = sum(w[2] for w in variable_windows)
        raise ImportTableError(
            "ANALYZER_VARIABLE_IMPORTS_UNSUPPORTED",
            f"import table declares {variable_count} variable imports; variable imports are not supported yet",
        )

    function_windows = [w for w in windows if w[1] > 0]
    if not function_windows:
        raise ImportTableError(
            "ANALYZER_IMPORT_TABLE_INVALID",
            "import stub table has no function windows",
        )

    # Pass 2: pairing regions. The primary region is the psp-fixup-imports one:
    # the named sections (.sceStub.text/.rodata.sceNid) when the input has them,
    # else the union of every window's runs. With named sections, a window whose
    # stub run and NID run both lie outside them is a detached run: the loader
    # patches its slots from its own NID run, so it is paired by that run alone
    # (grouped with overlapping runs that share its pairing offset). Without
    # section bounds a detached run cannot be told from a corrupted window, so
    # stripped inputs keep the single fail-closed region.
    st = elf.sec(".sceStub.text")
    nidsec = elf.sec(".rodata.sceNid")

    def sec_base(s):
        return s["addr"] + base if (base != 0 and s["addr"] < base) else s["addr"]

    section_stub = section_nid = None
    if st is not None:
        if st["size"] % 8:
            raise ValueError(".sceStub.text size is not a multiple of 8 (stub slots are 8 bytes)")
        section_stub = (sec_base(st), sec_base(st) + st["size"])
    if nidsec is not None:
        if nidsec["size"] % 4:
            raise ValueError(".rodata.sceNid size is not a multiple of 4")
        section_nid = (sec_base(nidsec), sec_base(nidsec) + nidsec["size"])

    primary_windows = function_windows
    detached = []
    if section_stub is not None or section_nid is not None:
        primary_windows = []
        for w in function_windows:
            stub_run = (w[4], w[4] + w[1] * 8)
            nid_run = (w[3], w[3] + w[1] * 4)
            if _spans_overlap(stub_run, section_stub) or _spans_overlap(nid_run, section_nid):
                primary_windows.append(w)
            else:
                detached.append(w)
        if not primary_windows:
            raise ImportTableError(
                "ANALYZER_IMPORT_TABLE_INVALID",
                "no import window lies in the named import sections",
            )
    regions = [_primary_pairing_region(primary_windows, section_stub, section_nid)]
    for group in _pairing_groups(detached):
        for run in _contiguous_runs(group):
            regions.append(_window_pairing_region(run))

    # Two pairing runs with different offsets must never claim one stub slot:
    # the slot would pair with two different NIDs.
    ordered = sorted(regions, key=lambda r: (r["stub_base"], r["stub_end"]))
    for left, right in zip(ordered, ordered[1:], strict=False):
        if right["stub_base"] < left["stub_end"]:
            raise ImportTableError(
                "ANALYZER_IMPORT_REGIONS_MISMATCH",
                f"import stub runs at 0x{left['stub_base']:08x} and "
                f"0x{right['stub_base']:08x} overlap but pair with different NID positions",
            )

    # Passes 3 and 4, per region: read the region's NID array once, lay window
    # claims over its positions, and emit one (stub_addr -> (library, NID)) pair
    # per slot by the region's pairing.
    stubs = {}
    findings = []
    for region in regions:
        prefix = "" if region["primary"] else f"import run at 0x{region['stub_base']:08x}: "
        region_stubs, region_findings = _pair_region(elf, region, prefix)
        stubs.update(region_stubs)
        findings.extend(region_findings)
    if detached:
        findings.append(
            "import windows outside the named import sections: "
            f"{len(detached)} windows, "
            f"{sum((r['stub_end'] - r['stub_base']) // 8 for r in regions[1:])} slots "
            "paired by their own runs")
    return stubs, findings


def _spans_overlap(a, b):
    """True when half-open span ``a`` shares an address with ``b`` (``b`` may be None)."""
    return b is not None and a[0] < b[1] and b[0] < a[1]


def _pairing_groups(windows):
    """Group function windows by pairing offset, in first-appearance order.

    Inside one pairing region stub slot p sits at stub_base + 8*p and its NID at
    nid_base + 4*p, so firstSym - 2*nidData is the same for every window of the
    region; windows with different offsets can never share a region.
    """
    groups = {}
    for w in windows:
        groups.setdefault(w[4] - 2 * w[3], []).append(w)
    return list(groups.values())


def _contiguous_runs(windows):
    """Split same-offset windows into runs whose stub slots touch or overlap.

    Each run keeps its windows in table order, so the last claimer of a slot
    stays the one later in the table.
    """
    runs = []  # [[table indices], stub end]
    order = sorted(range(len(windows)), key=lambda i: (windows[i][4], windows[i][1]))
    for i in order:
        w = windows[i]
        if runs and w[4] <= runs[-1][1]:
            runs[-1][0].append(i)
            runs[-1][1] = max(runs[-1][1], w[4] + w[1] * 8)
        else:
            runs.append([[i], w[4] + w[1] * 8])
    return [[windows[i] for i in sorted(indices)] for indices, _end in runs]


def _window_pairing_region(windows):
    """A pairing region bounded by the union of its windows' runs."""
    return dict(
        primary=False,
        windows=windows,
        stub_base=min(w[4] for w in windows),
        stub_end=max(w[4] + w[1] * 8 for w in windows),
        nid_base=min(w[3] for w in windows),
        nid_end=max(w[3] + w[1] * 4 for w in windows),
        finding=None,
    )


def _primary_pairing_region(windows, section_stub, section_nid):
    """The psp-fixup-imports pairing region and the windows in it.

    Prefer the real sections (the psp-fixup-imports pairing regions); fall back
    to the union of the windows' runs for a section the input does not name.
    """
    window_region = _window_pairing_region(windows)
    window_stub_base, window_stub_end = window_region["stub_base"], window_region["stub_end"]
    window_nid_base, window_nid_end = window_region["nid_base"], window_region["nid_end"]
    stub_base, stub_end = section_stub or (window_stub_base, window_stub_end)
    nid_base, nid_end = section_nid or (window_nid_base, window_nid_end)
    finding = None

    # Some retail inputs keep words in the named .rodata.sceNid section that no
    # window references: after the import-window run (a tail) or before it (a
    # head). The PSP fixup utility rejects that shape because it is asked to
    # rewrite the whole section, but the PSP loader reads only the NID words the
    # windows name, so static recompilation only needs those slots. Accept that
    # shape only when the window-derived regions are themselves 1:1, the stub run
    # starts at the stub section base, and both runs are fully contained by the
    # sections. Any inconsistent window remains a hard failure in _pair_region;
    # the unreferenced words are surfaced as a diagnostic.
    if (stub_end - stub_base != 2 * (nid_end - nid_base)
            and window_stub_end - window_stub_base == 2 * (window_nid_end - window_nid_base)
            and stub_base == window_stub_base
            and window_stub_end <= stub_end
            and nid_base <= window_nid_base
            and window_nid_end <= nid_end):
        slots = (window_stub_end - window_stub_base) // 8
        if nid_base == window_nid_base:
            finding = (
                "named import sections contain an unreferenced tail; using the "
                f"window-paired prefix ({slots} slots)"
            )
        else:
            finding = (
                "named import sections contain an unreferenced head of "
                f"{(window_nid_base - nid_base) // 4} NID words and a tail of "
                f"{(nid_end - window_nid_end) // 4} NID words; using the "
                f"window-paired run ({slots} slots)"
            )
        stub_base, stub_end = window_stub_base, window_stub_end
        nid_base, nid_end = window_nid_base, window_nid_end
    if stub_end - stub_base != 2 * (nid_end - nid_base):
        raise ImportTableError(
            "ANALYZER_IMPORT_REGIONS_MISMATCH",
            "import stub region size does not match NID region size "
            "(psp-fixup-imports requires stub slots to pair 1:1 with NIDs)"
        )
    return dict(
        primary=True,
        windows=windows,
        stub_base=stub_base,
        stub_end=stub_end,
        nid_base=nid_base,
        nid_end=nid_end,
        finding=finding,
    )


def _pair_region(elf, region, prefix):
    """Return (stubs, findings) for one pairing region; findings start with prefix."""
    stub_base, stub_end = region["stub_base"], region["stub_end"]
    nid_base, nid_end = region["nid_base"], region["nid_end"]
    stub_count = (stub_end - stub_base) // 8
    nid_count = (nid_end - nid_base) // 4
    if stub_count != nid_count or nid_count <= 0:
        raise ValueError(f"impossible import region: {stub_count} stub slots vs {nid_count} NIDs")

    nid_blob = elf.read_at_vaddr(nid_base, nid_count * 4)
    if nid_blob is None or len(nid_blob) != nid_count * 4:
        raise ValueError(f"truncated import NID region at 0x{nid_base:08x}")
    nids = struct.unpack(f"<{nid_count}I", nid_blob)

    claims = {}    # position -> library name (last claimer wins)
    claimers = {}  # position -> [libraries in table order]
    for libname, numFuncs, _numVars, nidData, firstSym in region["windows"]:
        if firstSym % 4:
            raise ValueError(f"import stub area 0x{firstSym:08x} is not 4-byte aligned")
        if firstSym + numFuncs * 8 > 0xFFFFFFFF or nidData + numFuncs * 4 > 0xFFFFFFFF:
            raise ValueError("import table address arithmetic exceeds 32-bit guest space")
        if firstSym < stub_base or (firstSym - stub_base) % 8:
            raise ValueError(
                f"import stub address 0x{firstSym:08x} is not an 8-byte slot of the stub region")
        if nidData < nid_base or (nidData - nid_base) % 4:
            raise ValueError(
                f"import NID table 0x{nidData:08x} is not a 4-byte slot of the NID region")
        first_pos = (firstSym - stub_base) // 8
        nid_pos = (nidData - nid_base) // 4
        if first_pos != nid_pos:
            raise ValueError(
                f"inconsistent import window {libname}: stub slot {first_pos} "
                f"but NID slot {nid_pos}")
        if first_pos + numFuncs > stub_count:
            raise ValueError(
                f"import window {libname} with {numFuncs} functions runs past "
                f"the stub region ({stub_count} slots)")
        for i in range(numFuncs):
            p = first_pos + i
            claims[p] = libname
            claimers.setdefault(p, []).append(libname)

    stubs = {}
    for p in range(nid_count):
        stubs[stub_base + p * 8] = (claims.get(p, UNATTRIBUTED_LIBRARY), nids[p])

    findings = []
    if region["finding"]:
        findings.append(region["finding"])
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


def _import_model(elf):
    """Return imports or a stable, fail-closed analyzer boundary."""
    try:
        return _import_model_impl(elf)
    except ImportTableError:
        raise
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
