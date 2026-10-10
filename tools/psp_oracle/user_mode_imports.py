#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Link-time gate: a user-mode PSP probe may import only user-mode libraries.

A user-mode module cannot link a kernel-only library. On a PSP-3000 a
user-mode PRX that imports one is refused at load time with 0x8002013C
(library not found), so no probe code runs and the launch is lost. The
oracle fixture Makefile runs this gate on every linked probe ELF, right after
psp-fixup-imports and before psp-prxgen, and the build fails when a user-mode
module imports a kernel-only library. The failure names each such library and
the NIDs imported from it.

A library is kernel-only when its name ends in ``_driver`` or ``ForKernel``
(the naming the firmware and the PSPSDK use for kernel exports), or when it
is listed in KERNEL_ONLY_LIBRARIES. A module is in kernel mode only when its
SceModuleInfo attribute has PSP_MODULE_KERNEL (0x1000); this rule does not
apply to kernel-mode modules, and the report says so. Every stub slot must
belong to a named library: a slot the parser cannot attribute fails the gate,
because its library cannot be shown to be a user library.

The registry is persistent console state, and on PSP-3000 6.6.1 even a
"read" open of a missing category creates it. Every probe, user or kernel
mode, may therefore import only the eight read-only sceReg calls listed in
REGISTRY_READ_NIDS; any other sceReg NID (set, create, remove, flush, exit or
an unknown one) fails the gate. ``--exact-library LIB=NID[,NID...]`` further
requires a module's imports from LIB to be exactly that set (the registry
cases pin their sceReg imports this way). ``--forbid-library LIB`` requires a
module to import nothing at all from LIB (the font-list case forbids every
sceReg import, read-only or not, so the probe itself can never open a
registry category).

Usage: user_mode_imports.py [--exact-library LIB=NID,...] [--forbid-library LIB]
       MODULE [MODULE ...]
Exit status: 0 when every module passes, 1 when a user-mode module imports a
kernel-only library or an unattributed stub, any module imports a registry
call outside the read-only set, an --exact-library set differs, or a
--forbid-library library is imported at all, 2 when a
module cannot be read, its import table is malformed, or the arguments are
malformed.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys

_TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

from psp_import_table import (  # noqa: E402
    UNATTRIBUTED_LIBRARY,
    ImportTable,
    ImportTableError,
    parse_import_table,
)

#: SceModuleInfo attribute bit of a kernel-mode module (PSPSDK PSP_MODULE_KERNEL).
PSP_MODULE_KERNEL = 0x1000

#: Library-name suffixes of kernel exports.
KERNEL_ONLY_SUFFIXES = ("_driver", "ForKernel")

#: Kernel-only libraries whose names carry neither suffix. The PSPSDK ships
#: sceUmd only as libpspumd_driver.a and libpspumd_kernel.a; user code imports
#: sceUmdUser instead.
KERNEL_ONLY_LIBRARIES = frozenset({"sceUmd"})

#: The registry library, and the only calls a probe may import from it: the
#: read-only calls named by the PSPSDK sceReg.S stubs (src/registry).
REGISTRY_LIBRARY = "sceReg"
REGISTRY_READ_NIDS = {
    0x92E41280: "sceRegOpenRegistry",
    0xFA8A5739: "sceRegCloseRegistry",
    0x1D8A762E: "sceRegOpenCategory",
    0x0CAE832B: "sceRegCloseCategory",
    0x2C0DB9DD: "sceRegGetKeysNum",
    0x2D211135: "sceRegGetKeys",
    0xD4475AA8: "sceRegGetKeyInfo",
    0x28A8E98A: "sceRegGetKeyValue",
}
#: Registry calls that write, or that the gate refuses for another reason;
#: named only so a failure says which call it found.
REGISTRY_REFUSED_NIDS = {
    0x39461B4D: "sceRegFlushRegistry",
    0x0D69BF40: "sceRegFlushCategory",
    0x4CA16893: "sceRegRemoveCategory",
    0x17768E14: "sceRegSetKeyValue",
    0x57641A81: "sceRegCreateKey",
    0x3615BC87: "sceRegRemoveKey",
    0xDEDA92BF: "sceRegRemoveRegistry",
    0x9B25EDF1: "sceRegExit",
    0xC5768D02: "sceRegGetKeyInfoByName",
    0x30BE0259: "sceRegGetKeyValueByName",
}

EXIT_OK = 0
EXIT_KERNEL_IMPORT = 1
EXIT_UNREADABLE = 2


def kernel_only_reason(library: str) -> str | None:
    """Return why ``library`` is kernel-only, or None for a user library."""
    for suffix in KERNEL_ONLY_SUFFIXES:
        if library.endswith(suffix):
            return f"name ends in {suffix}"
    if library in KERNEL_ONLY_LIBRARIES:
        return "known kernel-only library"
    return None


@dataclass(frozen=True)
class LibraryViolation:
    library: str
    reason: str
    nids: tuple[int, ...]


@dataclass(frozen=True)
class GateResult:
    module_attributes: int
    libraries: tuple[str, ...]
    violations: tuple[LibraryViolation, ...]

    @property
    def kernel_mode(self) -> bool:
        return bool(self.module_attributes & PSP_MODULE_KERNEL)

    @property
    def passed(self) -> bool:
        return not self.violations


def registry_call_name(nid: int) -> str:
    """Name a sceReg NID for a report (``unknown`` when the gate does not know it)."""
    return REGISTRY_READ_NIDS.get(nid) or REGISTRY_REFUSED_NIDS.get(nid) or "unknown"


def check_import_table(
    table: ImportTable,
    exact_libraries: dict[str, frozenset[int]] | None = None,
    forbidden_libraries: frozenset[str] = frozenset(),
) -> GateResult:
    """Apply the user-mode, read-only registry, exact-library and forbidden-library rules."""
    libraries = tuple(dict.fromkeys(func.library for func in table.funcs))
    nids_by_library: dict[str, list[int]] = {}
    for func in table.funcs:
        nids_by_library.setdefault(func.library, []).append(func.nid)
    violations = []
    if not table.module_attributes & PSP_MODULE_KERNEL:
        for library, nids in nids_by_library.items():
            if library == UNATTRIBUTED_LIBRARY:
                reason = "stub slots that no library window claims"
            else:
                reason = kernel_only_reason(library)
                if reason is None:
                    continue
            violations.append(LibraryViolation(library, reason, tuple(nids)))
    refused = tuple(nid for nid in nids_by_library.get(REGISTRY_LIBRARY, ())
                    if nid not in REGISTRY_READ_NIDS)
    if refused:
        names = ", ".join(registry_call_name(nid) for nid in refused)
        violations.append(LibraryViolation(
            REGISTRY_LIBRARY, f"registry calls outside the read-only set ({names})", refused))
    for library, expected in sorted((exact_libraries or {}).items()):
        found = frozenset(nids_by_library.get(library, ()))
        if found == expected:
            continue
        missing = sorted(expected - found)
        extra = sorted(found - expected)
        parts = []
        if missing:
            parts.append("missing " + ", ".join(f"0x{nid:08X}" for nid in missing))
        if extra:
            parts.append("unexpected " + ", ".join(f"0x{nid:08X}" for nid in extra))
        violations.append(LibraryViolation(
            library, f"imports differ from the --exact-library set ({'; '.join(parts)})",
            tuple(extra) or tuple(missing)))
    for library in sorted(forbidden_libraries):
        found = nids_by_library.get(library)
        if found:
            violations.append(LibraryViolation(
                library, "imports from a library this case forbids (--forbid-library)",
                tuple(found)))
    return GateResult(table.module_attributes, libraries, tuple(violations))


def check_module(
    data: bytes,
    exact_libraries: dict[str, frozenset[int]] | None = None,
    forbidden_libraries: frozenset[str] = frozenset(),
) -> GateResult:
    """Parse a linked PSP ELF/PRX image and apply the import rules."""
    return check_import_table(parse_import_table(data), exact_libraries, forbidden_libraries)


def parse_exact_library(value: str) -> tuple[str, frozenset[int]]:
    """Parse one ``LIB=NID[,NID...]`` argument; raise ValueError when malformed."""
    library, separator, raw = value.partition("=")
    if not separator or not library or not raw:
        raise ValueError(f"--exact-library must be LIB=NID[,NID...], got {value!r}")
    nids = []
    for item in raw.split(","):
        if not item.lower().startswith("0x") or len(item) != 10:
            raise ValueError(f"--exact-library NID must be 0x and 8 hex digits: {item!r}")
        nids.append(int(item, 16))
    if len(set(nids)) != len(nids):
        raise ValueError(f"--exact-library lists a NID twice: {value!r}")
    return library, frozenset(nids)


def format_result(name: str, result: GateResult) -> str:
    """Render one module's gate outcome as developer-facing text."""
    attributes = f"SceModuleInfo attribute 0x{result.module_attributes:04X}"
    listed = ", ".join(result.libraries) or "no imports"
    if result.kernel_mode and result.passed:
        return (f"user-mode import gate: {name}: kernel-mode module ({attributes}); "
                f"rule not applicable; imports: {listed}")
    if result.passed:
        return f"user-mode import gate: OK: {name}: imports only user libraries: {listed}"
    mode = "kernel-mode" if result.kernel_mode else "user-mode"
    lines = [
        f"user-mode import gate: FAILED: {name} is a {mode} module ({attributes}) "
        "that imports libraries a user-mode module cannot link, or calls the gate refuses:"
    ]
    for violation in result.violations:
        nids = ", ".join(f"0x{nid:08X}" for nid in violation.nids)
        lines.append(f"  {violation.library} ({violation.reason}): {nids}")
    lines.append(
        "  The PSP refuses to load such a PRX (0x8002013C, library not found). "
        "Import the user library that exports the same NIDs (for example "
        "sceDisplay instead of sceDisplay_driver), or keep the call out of this case."
    )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    exact: dict[str, frozenset[int]] = {}
    forbidden: set[str] = set()
    modules: list[str] = []
    while args:
        arg = args.pop(0)
        if arg == "--forbid-library":
            if not args or not args[0] or args[0].startswith("--"):
                print("user-mode import gate: --forbid-library needs LIB", file=sys.stderr)
                return EXIT_UNREADABLE
            forbidden.add(args.pop(0))
            continue
        if arg != "--exact-library":
            modules.append(arg)
            continue
        if not args:
            print("user-mode import gate: --exact-library needs LIB=NID[,NID...]",
                  file=sys.stderr)
            return EXIT_UNREADABLE
        try:
            library, nids = parse_exact_library(args.pop(0))
        except ValueError as exc:
            print(f"user-mode import gate: {exc}", file=sys.stderr)
            return EXIT_UNREADABLE
        if library in exact:
            print(f"user-mode import gate: --exact-library names {library} twice",
                  file=sys.stderr)
            return EXIT_UNREADABLE
        exact[library] = nids
    if not modules:
        print("usage: user_mode_imports.py [--exact-library LIB=NID,...] "
              "[--forbid-library LIB] MODULE [MODULE ...]", file=sys.stderr)
        return EXIT_UNREADABLE
    status = EXIT_OK
    for arg in modules:
        try:
            data = Path(arg).read_bytes()
            result = check_module(data, exact, frozenset(forbidden))
        except (OSError, ImportTableError) as exc:
            print(f"user-mode import gate: FAILED: {arg}: cannot read import table: {exc}",
                  file=sys.stderr)
            status = max(status, EXIT_UNREADABLE)
            continue
        text = format_result(arg, result)
        if result.passed:
            print(text)
        else:
            print(text, file=sys.stderr)
            status = max(status, EXIT_KERNEL_IMPORT)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
