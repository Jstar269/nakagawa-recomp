#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Shared data structures and bounded file-format validators for hst_doctor."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import shutil
import struct
import subprocess
from typing import Iterable, Sequence

EXPECTED_DISC_ID = "UCUS98701"
EXPECTED_ELF_MACHINE = 8  # EM_MIPS
EXPECTED_VFPU_FILES = {
    "vfpu_asin_lut65536.dat": 1536,
    "vfpu_asin_lut_deltas.dat": 517448,
    "vfpu_asin_lut_indices.dat": 798916,
    "vfpu_exp2_lut.dat": 262144,
    "vfpu_exp2_lut65536.dat": 512,
    "vfpu_log2_lut.dat": 2097152,
    "vfpu_log2_lut65536.dat": 516,
    "vfpu_log2_lut65536_quadratic.dat": 512,
    "vfpu_rcp_lut.dat": 262144,
    "vfpu_rsqrt_lut.dat": 262144,
    "vfpu_sin_lut8192.dat": 4100,
    "vfpu_sin_lut_delta.dat": 262144,
    "vfpu_sin_lut_exceptions.dat": 86938,
    "vfpu_sin_lut_interval_delta.dat": 131074,
    "vfpu_sqrt_lut.dat": 262144,
}
PRIVATE_PREFIXES = (
    "build/",
    "fs/",
    "logs/",
    "memstick/",
    "oracle/",
    "original_game/",
    "place_game_here/",
    "docs/opengrip_ref/",
    "opengrip_ref/",
    "OpenGrip_For_Inspiration/",
    "third_party/ghidra/exports/",
    "third_party/ghidra/projects/",
)
PRIVATE_EXTENSIONS = {
    ".at3",
    ".bin",
    ".chd",
    ".cso",
    ".dax",
    ".edat",
    ".elf",
    ".gim",
    ".iso",
    ".pbp",
    ".pmf",
    ".prx",
    ".psar",
    ".sfo",
    ".trace",
    ".vag",
}


@dataclass(frozen=True)
class Result:
    status: str
    code: str
    summary: str
    path: str | None = None
    detail: str | None = None
    remediation: str | None = None
    metadata: dict[str, object] = field(default_factory=dict)


class Report:
    def __init__(self, root: Path, scope: str) -> None:
        self.root = root
        self.scope = scope
        self.results: list[Result] = []

    def add(
        self,
        status: str,
        code: str,
        summary: str,
        *,
        path: Path | str | None = None,
        detail: str | None = None,
        remediation: str | None = None,
        metadata: dict[str, object] | None = None,
    ) -> None:
        display_path: str | None
        if isinstance(path, Path):
            try:
                display_path = path.resolve().relative_to(self.root.resolve()).as_posix()
            except (OSError, ValueError):
                display_path = str(path)
        elif path is None:
            display_path = None
        else:
            display_path = path
        self.results.append(
            Result(
                status=status,
                code=code,
                summary=summary,
                path=display_path,
                detail=detail,
                remediation=remediation,
                metadata=metadata or {},
            )
        )

    def pass_(self, code: str, summary: str, **kwargs: object) -> None:
        self.add("PASS", code, summary, **kwargs)

    def warn(self, code: str, summary: str, **kwargs: object) -> None:
        self.add("WARN", code, summary, **kwargs)

    def fail(self, code: str, summary: str, **kwargs: object) -> None:
        self.add("FAIL", code, summary, **kwargs)

    def info(self, code: str, summary: str, **kwargs: object) -> None:
        self.add("INFO", code, summary, **kwargs)

    def counts(self) -> dict[str, int]:
        counts = {name: 0 for name in ("PASS", "WARN", "FAIL", "INFO")}
        for result in self.results:
            counts[result.status] = counts.get(result.status, 0) + 1
        return counts

    def exit_code(self, strict: bool) -> int:
        counts = self.counts()
        if counts["FAIL"]:
            return 1
        if strict and counts["WARN"]:
            return 2
        return 0


def _read_prefix(path: Path, size: int) -> bytes:
    with path.open("rb") as stream:
        return stream.read(size)


def _run_version(command: Sequence[str], timeout: float = 8.0) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            list(command),
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 127, str(exc)
    output = (proc.stdout or proc.stderr).strip()
    first = output.splitlines()[0] if output else ""
    return proc.returncode, first


def _find_executable(names: Iterable[str], preferred_dir: Path | None = None) -> Path | None:
    if preferred_dir:
        for name in names:
            candidate = preferred_dir / name
            if candidate.is_file():
                return candidate
    for name in names:
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def _validate_pe_x64(path: Path) -> tuple[bool, str]:
    try:
        size = path.stat().st_size
        data = _read_prefix(path, 4096)
    except OSError as exc:
        return False, f"cannot read file: {exc}"
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False, "not a PE file (missing MZ header)"
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset < 0x40 or pe_offset > size - 6:
        return False, f"PE header offset 0x{pe_offset:x} is outside the {size}-byte file"
    try:
        if pe_offset + 6 > len(data):
            with path.open("rb") as stream:
                stream.seek(pe_offset)
                pe = stream.read(6)
        else:
            pe = data[pe_offset : pe_offset + 6]
    except OSError as exc:
        return False, f"cannot read PE header: {exc}"
    if len(pe) < 6 or pe[:4] != b"PE\0\0":
        return False, "invalid PE signature"
    machine = struct.unpack_from("<H", pe, 4)[0]
    if machine != 0x8664:
        return False, f"PE machine is 0x{machine:04x}, expected x86-64 (0x8664)"
    return True, "x86-64 PE"


def _parse_elf(path: Path) -> tuple[dict[str, int] | None, str | None]:
    try:
        size = path.stat().st_size
        header = _read_prefix(path, 64)
    except OSError as exc:
        return None, f"cannot read ELF: {exc}"
    if len(header) < 52 or header[:4] != b"\x7fELF":
        return None, "missing ELF magic or truncated ELF32 header"
    if header[4] != 1:
        return None, f"ELF class {header[4]} is not ELF32"
    if header[5] != 1:
        return None, f"ELF data encoding {header[5]} is not little-endian"
    try:
        e_type, e_machine, e_version = struct.unpack_from("<HHI", header, 16)
        entry, phoff, shoff = struct.unpack_from("<III", header, 24)
        ehsize, phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHHH", header, 40)
    except struct.error as exc:
        return None, f"truncated ELF header: {exc}"
    if e_machine != EXPECTED_ELF_MACHINE:
        return None, f"ELF machine {e_machine} is not MIPS ({EXPECTED_ELF_MACHINE})"
    if e_version != 1:
        return None, f"unsupported ELF version {e_version}"
    if ehsize < 52 or ehsize > size:
        return None, f"invalid ELF header size {ehsize} for file size {size}"
    if phnum:
        ph_end = phoff + phentsize * phnum
        if phentsize < 32 or ph_end < phoff or ph_end > size:
            return None, "program-header table is truncated or overflows the file"
    if shnum:
        sh_end = shoff + shentsize * shnum
        if shentsize < 40 or sh_end < shoff or sh_end > size:
            return None, "section-header table is truncated or overflows the file"
    loads = 0
    if phnum:
        try:
            with path.open("rb") as stream:
                for index in range(phnum):
                    stream.seek(phoff + index * phentsize)
                    ph = stream.read(32)
                    if len(ph) != 32:
                        return None, "truncated program header"
                    p_type, p_offset, _p_vaddr, _p_paddr, p_filesz, p_memsz, _p_flags, _p_align = struct.unpack(
                        "<8I", ph
                    )
                    if p_offset + p_filesz < p_offset or p_offset + p_filesz > size:
                        return None, f"program segment {index} extends beyond the file"
                    if p_memsz < p_filesz:
                        return None, f"program segment {index} has p_memsz < p_filesz"
                    if p_type == 1:
                        loads += 1
        except OSError as exc:
            return None, f"cannot read program headers: {exc}"
    if loads == 0:
        return None, "ELF contains no PT_LOAD segment"
    return {
        "size": size,
        "type": e_type,
        "machine": e_machine,
        "entry": entry,
        "phnum": phnum,
        "load_segments": loads,
        "shnum": shnum,
    }, None


def _parse_psp_header(path: Path) -> tuple[dict[str, object] | None, str | None]:
    try:
        data = _read_prefix(path, 0x80)
    except OSError as exc:
        return None, f"cannot read PSP header: {exc}"
    if len(data) < 0x64 or data[:4] != b"~PSP":
        return None, "missing ~PSP header or file is truncated"
    segment_count = data[0x27]
    if not 1 <= segment_count <= 4:
        return None, f"invalid PSP segment count {segment_count}"
    bss_size = struct.unpack_from("<I", data, 0x38)[0]
    segment_sizes = list(struct.unpack_from("<4I", data, 0x54))[:segment_count]
    if any(size == 0 for size in segment_sizes):
        return None, "one or more declared PSP segment sizes are zero"
    return {
        "segment_count": segment_count,
        "bss_size": bss_size,
        "segment_sizes": segment_sizes,
    }, None


def _validate_iso(path: Path) -> tuple[dict[str, object] | None, str | None]:
    try:
        size = path.stat().st_size
        if size < 18 * 2048:
            return None, f"file is too small to be an ISO9660 image ({size} bytes)"
        with path.open("rb") as stream:
            stream.seek(16 * 2048)
            pvd = stream.read(2048)
    except OSError as exc:
        return None, f"cannot read ISO: {exc}"
    if len(pvd) != 2048 or pvd[1:6] != b"CD001":
        return None, "sector 16 is not an ISO9660 volume descriptor"
    if pvd[0] != 1:
        return None, f"sector 16 descriptor type is {pvd[0]}, expected primary volume descriptor type 1"
    if pvd[6] != 1:
        return None, f"unsupported ISO9660 descriptor version {pvd[6]}"
    return {"size": size, "volume_descriptor_type": pvd[0], "volume_descriptor_version": pvd[6]}, None


def _scan_disc_id(path: Path, expected: str, limit: int = 128 * 1024 * 1024) -> bool:
    needle = expected.encode("ascii")
    overlap = len(needle) - 1
    scanned = 0
    tail = b""
    try:
        with path.open("rb") as stream:
            while scanned < limit:
                chunk = stream.read(min(4 * 1024 * 1024, limit - scanned))
                if not chunk:
                    break
                scanned += len(chunk)
                data = tail + chunk
                if needle in data:
                    return True
                tail = data[-overlap:] if overlap else b""
    except OSError:
        return False
    return False


def _bounded_nonempty_directory(path: Path, limit: int = 100_000) -> tuple[int, str | None]:
    count = 0
    try:
        for entry in path.rglob("*"):
            if entry.is_file():
                count += 1
                if count >= limit:
                    return count, None
    except OSError as exc:
        return count, str(exc)
    return count, None
