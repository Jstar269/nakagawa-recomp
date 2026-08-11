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
# Usage: imports.py <prx-elf> <base-hex> [--toml out.toml]

import json
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from analyze import Elf


# Guest strings are metadata, not trusted host-language source. PSP import-library names in
# practice are tiny (sceKernelLibrary, sceDisplay, etc.); keep a generous hard ceiling so a
# malformed/unmapped string cannot make the offline pipeline walk an unbounded address range.
MAX_IMPORT_LIBRARY_NAME_BYTES = 1024

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
            raise ValueError(f"guest string at 0x{addr:08x} leaves mapped input")
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


def _import_model(elf):
    """Return (stubs, findings) for a PSP ELF import table.

    stubs maps every stub-slot address in the region to
    (library name, NID); slots no window claims map to the
    UNATTRIBUTED_LIBRARY marker. findings is a list of deterministic
    structural strings (unclaimed and multi-claimed positions).
    """
    mi = elf.sec(".rodata.sceModuleInfo")
    if not mi:
        raise SystemExit("no .rodata.sceModuleInfo section")
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

    def r32(a):
        b4 = elf.read_at_vaddr(a, 4)
        if b4 is None or len(b4) != 4:
            raise ValueError(f"truncated import NID at 0x{a:08x}")
        return struct.unpack("<I", b4)[0]

    # Pass 1: walk the PspLibStubEntry window table (libstub..libstubend).
    windows = []  # (library name, numFuncs, nidData, firstSym)
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
            raise ValueError(f"import entry at 0x{pos:08x}: {numFuncs} functions but null NID table pointer")
        libname = _read_guest_cstr(elf, name_ptr) if name_ptr else "(null)"
        windows.append((libname, numFuncs, nidData, firstSym))
        step = size * 4
        if step <= 0 or pos + step > 0xFFFFFFFF:
            raise ValueError("import stub table step wraps 32-bit guest space")
        pos += step
    if not windows:
        raise ValueError("import stub table is empty")

    # Pass 2: full stub/NID region extents. Prefer the real sections (the
    # psp-fixup-imports pairing regions); fall back to the union of the
    # window runs for stripped/derived inputs.
    st = elf.sec(".sceStub.text")
    nidsec = elf.sec(".rodata.sceNid")

    def sec_base(s):
        return s["addr"] + base if (base != 0 and s["addr"] < base) else s["addr"]

    stub_base = stub_end = None
    nid_base = nid_end = None
    function_windows = [w for w in windows if w[1] > 0]
    if not function_windows:
        raise ValueError("import stub table has no function windows")
    window_stub_base = min(w[3] for w in function_windows)
    window_stub_end = max(w[3] + w[1] * 8 for w in function_windows)
    window_nid_base = min(w[2] for w in function_windows)
    window_nid_end = max(w[2] + w[1] * 4 for w in function_windows)
    section_tail_finding = None
    if st is not None:
        if st["size"] % 8:
            raise ValueError(".sceStub.text size is not a multiple of 8 (stub slots are 8 bytes)")
        stub_base, stub_end = sec_base(st), sec_base(st) + st["size"]
    if nidsec is not None:
        if nidsec["size"] % 4:
            raise ValueError(".rodata.sceNid size is not a multiple of 4")
        nid_base, nid_end = sec_base(nidsec), sec_base(nidsec) + nidsec["size"]
    if stub_base is None:
        stub_base, stub_end = window_stub_base, window_stub_end
    if nid_base is None:
        nid_base, nid_end = window_nid_base, window_nid_end

    # Some retail-style ET_EXEC inputs keep auxiliary/unreferenced NIDs in the
    # named .rodata.sceNid section after the import-window prefix.  The PSP
    # fixup utility quite reasonably rejects that shape because it is asked to
    # rewrite the whole section, but static recompilation only needs the slots
    # actually named by SceModuleInfo.  Accept that compatibility shape only
    # when both window-derived regions are themselves 1:1, start at the named
    # section bases, and are fully contained by the sections.  Any inconsistent
    # window remains a hard failure below; the tail is surfaced as a diagnostic.
    if (stub_end - stub_base != 2 * (nid_end - nid_base)
            and window_stub_end - window_stub_base == 2 * (window_nid_end - window_nid_base)
            and stub_base == window_stub_base
            and nid_base == window_nid_base
            and window_stub_end <= stub_end
            and window_nid_end <= nid_end):
        section_tail_finding = (
            "named import sections contain an unreferenced tail; using the "
            f"window-paired prefix ({(window_stub_end - window_stub_base) // 8} slots)"
        )
        stub_base, stub_end = window_stub_base, window_stub_end
        nid_base, nid_end = window_nid_base, window_nid_end
    if stub_end - stub_base != 2 * (nid_end - nid_base):
        raise ValueError(
            "import stub region size does not match NID region size "
            "(psp-fixup-imports requires stub slots to pair 1:1 with NIDs)"
        )
    stub_count = (stub_end - stub_base) // 8
    nid_count = (nid_end - nid_base) // 4
    if stub_count != nid_count or nid_count <= 0:
        raise ValueError(f"impossible import region: {stub_count} stub slots vs {nid_count} NIDs")

    # Pass 3: read the global NID array once, then lay window claims over positions.
    nid_blob = elf.read_at_vaddr(nid_base, nid_count * 4)
    if nid_blob is None or len(nid_blob) != nid_count * 4:
        raise ValueError(f"truncated import NID region at 0x{nid_base:08x}")
    nids = struct.unpack(f"<{nid_count}I", nid_blob)

    claims = {}    # position -> library name (last claimer wins)
    claimers = {}  # position -> [libraries in table order]
    for libname, numFuncs, nidData, firstSym in windows:
        if numFuncs == 0:
            continue
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

    # Pass 4: emit one (stub_addr -> (library, NID)) pair per slot by global pairing.
    stubs = {}
    for p in range(nid_count):
        stubs[stub_base + p * 8] = (claims.get(p, UNATTRIBUTED_LIBRARY), nids[p])

    findings = []
    if section_tail_finding:
        findings.append(section_tail_finding)
    unclaimed = [p for p in range(nid_count) if p not in claims]
    if unclaimed:
        findings.append(
            f"stub slots not covered by any library window: {len(unclaimed)} "
            f"positions {unclaimed}")
    ambiguous = sorted(p for p, libs in claimers.items() if len(libs) > 1)
    if ambiguous:
        findings.append(
            f"stub slots claimed by multiple library windows: {len(ambiguous)} "
            f"positions {ambiguous}")
    return stubs, findings


def parse_imports(elf):
    """Return {stub address: (library name, NID)} for every import stub slot."""
    stubs, _findings = _import_model(elf)
    return stubs


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        sys.stderr.write("usage: imports.py <prx-elf> <base-hex> [--toml out.toml]\n")
        return 2
    elf = Elf(args[0], base=int(args[1], 16))
    stubs, findings = _import_model(elf)

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
