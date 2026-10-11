#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Check format specifier portability across portable core C source files.

Simulates a 32-bit newlib host target (such as devkitARM / ARMv6k) where
uint32_t is unsigned long and int32_t is long. Under gcc -Wformat -Werror=format,
using %u / %x / %d for 32-bit fixed-width types fails when uint32_t is
unsigned long instead of unsigned int.

This checker compiles each portable core file with -fsyntax-only -Wformat
-Werror=format against a generated stdint.h / inttypes.h shim directory located
under build/, verifying that all sites use standard <inttypes.h> PRI macros or
explicit casts.

The shim is force-included (-include) so it precedes every system header: glibc's
<sys/types.h> would otherwise typedef int8_t..int64_t with the host's widths and
conflict with the shim, so the shim defines glibc's guards for those typedefs.
The shim is only faithful where `long` is 32 bits wide, as it is on every 32-bit
newlib target and on Windows: the core pins CpuState offsets and sizes with static
assertions, so a 64-bit `long` would fail them for the wrong reason. On an LP64
host the check therefore compiles with -m32, which needs only the 32-bit C library
headers (libc6-dev-i386 on Debian/Ubuntu; gcc-multilib also provides them) because
nothing is linked, and fails closed with exit status 2 when that is unavailable,
rather than passing vacuously.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

DEFAULT_CORE_SRCS = [
    "src/rt/recomp.c",
    "src/rt/flight_recorder.c",
    "src/rt/cpu_lle.c",
    "src/rt/domain_mode.c",
    "src/rt/nested_frames.c",
    "src/rt/stale_code.c",
    "src/rt/guest_interp.c",
    "src/rt/title_config.c",
    "src/rt/vfpu_tables.c",
    "src/rt/archive_vfs.c",
    "src/rt/debug.c",
    "src/rt/watchpoints_file.c",
    "src/rt/guest_printf.c",
    "src/rt/perf.c",
    "src/rt/vfpu_interp.c",
    "src/rt/iso_public.c",
    "src/rt/pgd_unavailable.c",
    "src/rt/mpeg.c",
    "src/rt/pgf_public.c",
    "src/rt/savedata.c",
    "src/rt/ge.c",
    "src/rt/h264_null.c",
    "src/rt/sr_coro.c",
]

STDINT_SHIM = """/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic 32-bit newlib stdint.h shim for format portability check */
#ifndef _FORMAT_SHIM_STDINT_H
#define _FORMAT_SHIM_STDINT_H

/* glibc typedefs int8_t..int64_t and uint8_t..uint64_t in bits/stdint-intn.h and
 * bits/stdint-uintn.h (older releases: inside <sys/types.h> behind __int8_t_defined),
 * and <sys/types.h> includes them with the host widths. Defining their guards here
 * keeps this shim authoritative; other libcs never test these names. */
#define _BITS_STDINT_INTN_H 1
#define _BITS_STDINT_UINTN_H 1
#define __int8_t_defined 1

typedef signed char        int8_t;
typedef short              int16_t;
typedef long               int32_t;
typedef long long          int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned long      uint32_t;
typedef unsigned long long uint64_t;

#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
#ifdef __INTPTR_TYPE__
typedef __INTPTR_TYPE__    intptr_t;
#else
typedef long long          intptr_t;
#endif
#endif

#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
#ifdef __UINTPTR_TYPE__
typedef __UINTPTR_TYPE__   uintptr_t;
#else
typedef unsigned long long uintptr_t;
#endif
#endif

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535
#define INT32_MIN  (-2147483647L - 1)
#define INT32_MAX  2147483647L
#define UINT32_MAX 4294967295UL
#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL

#define INT8_C(c)   c
#define UINT8_C(c)  c
#define INT16_C(c)  c
#define UINT16_C(c) c
#define INT32_C(c)  c ## L
#define UINT32_C(c) c ## UL
#define INT64_C(c)  c ## LL
#define UINT64_C(c) c ## ULL
#define INTMAX_C(c) c ## LL
#define UINTMAX_C(c) c ## ULL

#endif /* _FORMAT_SHIM_STDINT_H */
"""

INTTYPES_SHIM = """/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic 32-bit newlib inttypes.h shim for format portability check */
#ifndef _FORMAT_SHIM_INTTYPES_H
#define _FORMAT_SHIM_INTTYPES_H

#include <stdint.h>

#define PRId8   "d"
#define PRIi8   "i"
#define PRIo8   "o"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"

#define PRId16  "d"
#define PRIi16  "i"
#define PRIo16  "o"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"

#define PRId32  "ld"
#define PRIi32  "li"
#define PRIo32  "lo"
#define PRIu32  "lu"
#define PRIx32  "lx"
#define PRIX32  "lX"

#define PRId64  "lld"
#define PRIi64  "lli"
#define PRIo64  "llo"
#define PRIu64  "llu"
#define PRIx64  "llx"
#define PRIX64  "llX"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#define SCNd32  "ld"
#define SCNi32  "li"
#define SCNo32  "lo"
#define SCNu32  "lu"
#define SCNx32  "lx"

#define SCNd64  "lld"
#define SCNi64  "lli"
#define SCNo64  "llo"
#define SCNu64  "llu"
#define SCNx64  "llx"

#endif /* _FORMAT_SHIM_INTTYPES_H */
"""


def generate_shim_headers(shim_dir: Path) -> Path:
    """Write synthetic stdint.h and inttypes.h shims into shim_dir/include."""
    include_dir = shim_dir / "include"
    include_dir.mkdir(parents=True, exist_ok=True)
    (include_dir / "stdint.h").write_text(STDINT_SHIM, encoding="utf-8")
    (include_dir / "inttypes.h").write_text(INTTYPES_SHIM, encoding="utf-8")
    return include_dir


def ensure_title_config_header(repo_root: Path) -> Path:
    """Ensure build/title-config/generic/sr_title_config.h exists."""
    target = repo_root / "build" / "title-config" / "generic" / "sr_title_config.h"
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        gen_script = repo_root / "tools" / "title_runtime_config.py"
        if gen_script.exists():
            subprocess.run(
                [sys.executable, str(gen_script), "--output", str(target)],
                check=True,
                capture_output=True,
            )
        else:
            target.write_text(
                "#ifndef SR_TITLE_CONFIG_GENERATED_H\n"
                "#define SR_TITLE_CONFIG_GENERATED_H\n"
                "#endif\n",
                encoding="utf-8",
            )
    return target.parent


def resolve_compiler(compiler: str) -> str:
    """Resolve compiler path with fallback to MSYS2 UCRT64 on Windows."""
    resolved = shutil.which(compiler)
    if resolved:
        return resolved
    if sys.platform == "win32" and compiler == "gcc":
        ucrt_gcc = Path(r"C:\msys64\ucrt64\bin\gcc.exe")
        if ucrt_gcc.exists():
            return str(ucrt_gcc)
    return compiler


class ToolchainUnavailable(RuntimeError):
    """The host compiler cannot produce the 32-bit `long` the check needs."""


def host_long_width(compiler: str) -> int:
    """Width of `long` in bytes for the compiler's default target, from its predefined macros."""
    proc = subprocess.run(
        [compiler, "-dM", "-E", "-x", "c", "-"],
        input="",
        capture_output=True,
        text=True,
    )
    match = re.search(r"^#define __SIZEOF_LONG__ (\d+)$", proc.stdout, re.M)
    if proc.returncode != 0 or not match:
        raise ToolchainUnavailable(f"{compiler} did not report __SIZEOF_LONG__")
    return int(match.group(1))


def narrow_long_flags(long_width: int) -> list[str]:
    """Flags that give the check build a 32-bit `long`: nothing on ILP32/LLP64, -m32 on LP64."""
    return ["-m32"] if long_width == 8 else []


def verify_narrow_long(compiler: str, flags: list[str], scratch_dir: Path) -> None:
    """Compile a one-line translation unit with the chosen flags; -m32 needs the 32-bit libc headers."""
    if not flags:
        return
    probe = scratch_dir / "narrow_long_probe.c"
    probe.write_text("#include <stdio.h>\nint probe(void) { return (int)sizeof(long); }\n", encoding="utf-8")
    proc = subprocess.run(
        [compiler, *flags, "-fsyntax-only", "-std=c11", str(probe)],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise ToolchainUnavailable(
            f"{compiler} cannot compile with {' '.join(flags)} (install the 32-bit C library headers: "
            f"libc6-dev-i386 on Debian/Ubuntu, or gcc-multilib): {proc.stderr.strip()}"
        )


def get_core_sources(repo_root: Path) -> list[str]:
    """Return default 23 portable core files, adapting to private alternatives if present."""
    sources: list[str] = []
    for item in DEFAULT_CORE_SRCS:
        if item == "src/rt/iso_public.c" and (repo_root / "src/rt/iso.c").exists():
            sources.append("src/rt/iso.c")
        elif item == "src/rt/pgd_unavailable.c" and (repo_root / "src/rt/pgd.c").exists():
            sources.append("src/rt/pgd.c")
        elif item == "src/rt/pgf_public.c" and (repo_root / "src/rt/pgf.c").exists():
            sources.append("src/rt/pgf.c")
        else:
            sources.append(item)
    return sources


def check_format_portability(
    files: list[str] | None = None,
    shim_dir: Path | None = None,
    compiler: str = "gcc",
    verbose: bool = False,
    repo_root: Path | None = None,
    narrow_long: bool = True,
) -> tuple[int, list[str]]:
    """Check C files against synthetic 32-bit newlib format shim.

    narrow_long=False skips the 32-bit `long` requirement; it is for synthetic files
    without width-pinned layouts, never for the core.

    Returns:
        (failed_count, list_of_error_outputs)

    Raises:
        ToolchainUnavailable: the compiler cannot give the check build a 32-bit `long`.
    """
    if repo_root is None:
        repo_root = Path(__file__).resolve().parent.parent

    if shim_dir is None:
        shim_dir = repo_root / "build" / "format_shim"

    shim_inc = generate_shim_headers(shim_dir)
    generic_cfg_dir = ensure_title_config_header(repo_root)

    if not files:
        files = get_core_sources(repo_root)

    resolved_cc = resolve_compiler(compiler)
    width_flags: list[str] = []
    if narrow_long:
        width_flags = narrow_long_flags(host_long_width(resolved_cc))
        verify_narrow_long(resolved_cc, width_flags, shim_dir)
    failed_count = 0
    errors: list[str] = []

    for file_str in files:
        file_path = repo_root / file_str if not Path(file_str).is_absolute() else Path(file_str)
        if not file_path.exists():
            msg = f"ERROR: File not found: {file_str}"
            errors.append(msg)
            failed_count += 1
            if verbose:
                print(msg, file=sys.stderr)
            continue

        cmd = [
            resolved_cc,
            *width_flags,
            "-fsyntax-only",
            "-Wformat",
            "-Werror=format",
            "-D_GNU_SOURCE",
            "-std=c11",
            "-DSR_FLIGHT_RECORDER_LINKED",
            "-isystem",
            str(shim_inc),
            # The shim must precede every system header (see the module docstring).
            "-include",
            str(shim_inc / "stdint.h"),
            "-I" + str(repo_root / "src" / "rt"),
            "-I" + str(repo_root / "src" / "core"),
            "-I" + str(repo_root / "src" / "core" / "generated"),
            "-I" + str(repo_root / "src" / "player"),
            "-I" + str(generic_cfg_dir),
            str(file_path),
        ]

        if verbose:
            print(f"Checking {file_str}...")

        proc = subprocess.run(
            cmd,
            cwd=str(repo_root),
            capture_output=True,
            text=True,
        )

        if proc.returncode != 0:
            failed_count += 1
            output = proc.stderr.strip() or proc.stdout.strip()
            msg = f"FAIL: {file_str}\n{output}"
            errors.append(msg)
            if verbose:
                print(msg, file=sys.stderr)

    return failed_count, errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify 32-bit format specifier portability across portable core sources."
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="Optional specific C files to check (default: all 23 portable core files)",
    )
    parser.add_argument(
        "--shim-dir",
        type=Path,
        default=None,
        help="Directory to place generated stdint/inttypes shims",
    )
    parser.add_argument(
        "--compiler",
        default="gcc",
        help="C compiler binary to invoke (default: gcc)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Print verbose diagnostic details",
    )
    parser.add_argument(
        "--allow-wide-long",
        action="store_true",
        help="Skip the 32-bit long requirement (synthetic files only; the core's layout pins need it)",
    )

    args = parser.parse_args(argv)
    repo_root = Path(__file__).resolve().parent.parent

    target_files = args.files if args.files else get_core_sources(repo_root)
    try:
        failed, errors = check_format_portability(
            files=target_files,
            shim_dir=args.shim_dir,
            compiler=args.compiler,
            verbose=args.verbose,
            repo_root=repo_root,
            narrow_long=not args.allow_wide_long,
        )
    except ToolchainUnavailable as exc:
        print(f"format-portability-check: TOOLCHAIN_UNAVAILABLE {exc}", file=sys.stderr)
        return 2

    if failed == 0:
        print(f"format-portability-check: {len(target_files)}/{len(target_files)} files clean (0 format errors)")
        return 0

    print(
        f"format-portability-check: {failed}/{len(target_files)} files failed format check",
        file=sys.stderr,
    )
    for err in errors:
        print(err, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
