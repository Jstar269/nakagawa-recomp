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

Usage: user_mode_imports.py MODULE [MODULE ...]
Exit status: 0 when every module passes, 1 when a user-mode module imports a
kernel-only library or an unattributed stub, 2 when a module cannot be read or
its import table is malformed.
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


def check_import_table(table: ImportTable) -> GateResult:
    """Apply the user-mode import rule to one parsed import table."""
    libraries = tuple(dict.fromkeys(func.library for func in table.funcs))
    if table.module_attributes & PSP_MODULE_KERNEL:
        return GateResult(table.module_attributes, libraries, ())
    nids_by_library: dict[str, list[int]] = {}
    for func in table.funcs:
        nids_by_library.setdefault(func.library, []).append(func.nid)
    violations = []
    for library, nids in nids_by_library.items():
        if library == UNATTRIBUTED_LIBRARY:
            reason = "stub slots that no library window claims"
        else:
            reason = kernel_only_reason(library)
            if reason is None:
                continue
        violations.append(LibraryViolation(library, reason, tuple(nids)))
    return GateResult(table.module_attributes, libraries, tuple(violations))


def check_module(data: bytes) -> GateResult:
    """Parse a linked PSP ELF/PRX image and apply the user-mode import rule."""
    return check_import_table(parse_import_table(data))


def format_result(name: str, result: GateResult) -> str:
    """Render one module's gate outcome as developer-facing text."""
    attributes = f"SceModuleInfo attribute 0x{result.module_attributes:04X}"
    listed = ", ".join(result.libraries) or "no imports"
    if result.kernel_mode:
        return (f"user-mode import gate: {name}: kernel-mode module ({attributes}); "
                f"rule not applicable; imports: {listed}")
    if result.passed:
        return f"user-mode import gate: OK: {name}: imports only user libraries: {listed}"
    lines = [
        f"user-mode import gate: FAILED: {name} is a user-mode module ({attributes}) "
        "that imports libraries a user-mode module cannot link:"
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
    args = sys.argv[1:] if argv is None else argv
    if not args:
        print("usage: user_mode_imports.py MODULE [MODULE ...]", file=sys.stderr)
        return EXIT_UNREADABLE
    status = EXIT_OK
    for arg in args:
        try:
            data = Path(arg).read_bytes()
            result = check_module(data)
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
