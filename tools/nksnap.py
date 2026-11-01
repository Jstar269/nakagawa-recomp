# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Read and write `nksnap` v1 snapshot files and parse the snapshot runner's verdict lines.

A snapshot is one guest call's starting state: the full ``CpuState``, the entry PC and the
guest memory blocks the call can touch. ``src/rt/nksnap.c`` is the runtime's loader and
writer for the same format; ``src/rt/snapshot_runner.c`` runs a snapshot through the
translated code and the reference interpreter in ``src/ref`` and prints one verdict line.
Snapshots hold guest memory, so they stay private like traces and are never committed.

Layout, all integers little-endian::

    8 bytes   magic "NKSNAP01"
    u32       SR_CPUSTATE_ABI_VERSION (src/rt/recomp.h)
    u32       sizeof(CpuState)
    N bytes   the raw CpuState
    u32       entry PC
    u32       block count
    per block u32 guest address, u32 size, size bytes

The loaders refuse a file with a named reason (``REASONS``); this module and the C loader
use the same names, and ``tools/test_snapshot_format.py`` pins both.
"""

from __future__ import annotations

import re
import struct
import dataclasses
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"NKSNAP01"
#: SR_CPUSTATE_ABI_VERSION and sizeof(CpuState) in src/rt/recomp.h (pinned by the test).
ABI_VERSION = 3
CPUSTATE_SIZE = 1000

# CpuState field offsets (src/rt/recomp.h; the _Static_asserts there pin the tail).
OFF_R = 0
OFF_HI = 128
OFF_LO = 132
OFF_PC = 136
OFF_F = 140
OFF_FCR31 = 268
OFF_FPCOND = 272
OFF_V = 276
OFF_VFPU_CTRL = 788
OFF_COP0 = 852
OFF_NEXT_PC = 980
OFF_IN_DELAY_SLOT = 984
OFF_FLOW_KIND = 988
OFF_FLOW_TARGET = 992
OFF_LLBIT = 996

#: The guest regions the runtime's memory arena models: (first address, size, name).
#: A block must lie wholly inside one of them, at its unmirrored address.
GUEST_REGIONS = (
    (0x00010000, 0x00004000, "scratchpad"),
    (0x04000000, 0x00200000, "vram"),
    (0x08000000, 0x04000000, "ram"),
)

#: Every refusal reason, in the order the loader checks them.
REASONS = (
    "bad-magic",
    "abi-version",
    "cpustate-size",
    "truncated",
    "block-size",
    "block-overlap",
    "block-outside-guest",
    "trailing-bytes",
)


class SnapshotError(ValueError):
    """A snapshot the loader refuses; ``reason`` is one of ``REASONS``."""

    def __init__(self, reason: str, detail: str = "") -> None:
        if reason not in REASONS:
            raise AssertionError(f"unknown snapshot refusal reason {reason!r}")
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason
        self.detail = detail


@dataclass(frozen=True)
class Block:
    addr: int
    data: bytes


@dataclass
class Snapshot:
    cpu: bytes
    entry_pc: int
    blocks: list[Block] = dataclasses.field(default_factory=list)


def cpu_state(
    *,
    r: dict[int, int] | None = None,
    hi: int = 0,
    lo: int = 0,
    f: dict[int, int] | None = None,
    fcr31: int = 0,
) -> bytes:
    """A raw CpuState with the given GPRs, HI/LO, FPU registers (raw bits) and FCR31.

    The VFPU control registers take their reset values (prefixes 0xe4, as the runtime
    seeds a fresh thread); every other field is zero.
    """
    raw = bytearray(CPUSTATE_SIZE)
    for index, value in (r or {}).items():
        if not 0 <= index < 32:
            raise ValueError(f"GPR index {index} out of range")
        struct.pack_into("<I", raw, OFF_R + 4 * index, value & 0xFFFFFFFF)
    struct.pack_into("<II", raw, OFF_HI, hi & 0xFFFFFFFF, lo & 0xFFFFFFFF)
    for index, value in (f or {}).items():
        if not 0 <= index < 32:
            raise ValueError(f"FPU register index {index} out of range")
        struct.pack_into("<I", raw, OFF_F + 4 * index, value & 0xFFFFFFFF)
    struct.pack_into("<I", raw, OFF_FCR31, fcr31 & 0xFFFFFFFF)
    struct.pack_into("<II", raw, OFF_VFPU_CTRL, 0xE4, 0xE4)
    return bytes(raw)


def gpr(cpu: bytes, index: int) -> int:
    return struct.unpack_from("<I", cpu, OFF_R + 4 * index)[0]


def _region_of(addr: int, size: int) -> str | None:
    for first, length, name in GUEST_REGIONS:
        if first <= addr and addr + size <= first + length:
            return name
    return None


def validate_blocks(blocks: list[Block]) -> None:
    """Raise SnapshotError for a zero or overflowing size, an overlap, or an unmodelled range."""
    for block in blocks:
        size = len(block.data)
        if size == 0 or block.addr + size > 0x1_0000_0000:
            raise SnapshotError("block-size", f"block at 0x{block.addr:08x} size 0x{size:x}")
    ordered = sorted(blocks, key=lambda b: b.addr)
    for previous, current in zip(ordered, ordered[1:], strict=False):
        if current.addr < previous.addr + len(previous.data):
            raise SnapshotError(
                "block-overlap",
                f"0x{previous.addr:08x}+0x{len(previous.data):x} overlaps 0x{current.addr:08x}",
            )
    for block in blocks:
        if _region_of(block.addr, len(block.data)) is None:
            raise SnapshotError(
                "block-outside-guest", f"0x{block.addr:08x}+0x{len(block.data):x}"
            )


def pack_raw(
    cpu: bytes,
    entry_pc: int,
    blocks: list[tuple[int, int, bytes]],
    *,
    magic: bytes = MAGIC,
    abi_version: int = ABI_VERSION,
    cpu_size: int | None = None,
) -> bytes:
    """Serialize without validation, so a test can build each malformed form.

    *blocks* are ``(addr, declared size, bytes)`` triples; the declared size need not
    match the bytes.
    """
    out = bytearray(magic)
    out += struct.pack("<II", abi_version, len(cpu) if cpu_size is None else cpu_size)
    out += cpu
    out += struct.pack("<II", entry_pc & 0xFFFFFFFF, len(blocks))
    for addr, size, data in blocks:
        out += struct.pack("<II", addr & 0xFFFFFFFF, size & 0xFFFFFFFF)
        out += data
    return bytes(out)


def pack(snapshot: Snapshot) -> bytes:
    """Serialize a valid snapshot; refuses what the loader would refuse."""
    if len(snapshot.cpu) != CPUSTATE_SIZE:
        raise SnapshotError("cpustate-size", f"{len(snapshot.cpu)} bytes")
    validate_blocks(snapshot.blocks)
    return pack_raw(
        snapshot.cpu,
        snapshot.entry_pc,
        [(b.addr, len(b.data), b.data) for b in snapshot.blocks],
    )


def parse(data: bytes) -> Snapshot:
    """Parse and validate one snapshot, raising SnapshotError with the loader's reason."""
    if len(data) < len(MAGIC):
        raise SnapshotError("truncated", "shorter than the magic")
    if data[: len(MAGIC)] != MAGIC:
        raise SnapshotError("bad-magic")
    pos = len(MAGIC)

    def take(count: int, what: str) -> bytes:
        nonlocal pos
        if len(data) - pos < count:
            raise SnapshotError("truncated", f"{what} at offset {pos}")
        chunk = data[pos : pos + count]
        pos += count
        return chunk

    (abi,) = struct.unpack("<I", take(4, "ABI version"))
    if abi != ABI_VERSION:
        raise SnapshotError("abi-version", f"file {abi}, runtime {ABI_VERSION}")
    (cpu_size,) = struct.unpack("<I", take(4, "CpuState size"))
    if cpu_size != CPUSTATE_SIZE:
        raise SnapshotError("cpustate-size", f"file {cpu_size}, runtime {CPUSTATE_SIZE}")
    cpu = take(CPUSTATE_SIZE, "CpuState")
    entry_pc, count = struct.unpack("<II", take(8, "entry PC and block count"))
    blocks: list[Block] = []
    for index in range(count):
        addr, size = struct.unpack("<II", take(8, f"block {index} header"))
        if size == 0 or addr + size > 0x1_0000_0000:
            raise SnapshotError("block-size", f"block {index} at 0x{addr:08x} size 0x{size:x}")
        blocks.append(Block(addr, take(size, f"block {index} bytes")))
    if pos != len(data):
        raise SnapshotError("trailing-bytes", f"{len(data) - pos} byte(s) after the last block")
    validate_blocks(blocks)
    return Snapshot(cpu, entry_pc, blocks)


def read(path: Path) -> Snapshot:
    return parse(Path(path).read_bytes())


def write(path: Path, snapshot: Snapshot) -> None:
    Path(path).write_bytes(pack(snapshot))


# ---- verdict grammar --------------------------------------------------------------------

#: Stop kinds a MISMATCH on ``stop`` names.
STOP_KINDS = ("return", "syscall", "import")

#: NOT_CHECKED reasons: the comparison could not be made, which is never a pass.
NOT_CHECKED_REASONS = (
    "ref-no-vfpu",            # the reference has no VFPU; pc= op= name the instruction
    "ref-unimplemented",      # the reference lacks this form; pc= op=
    "ref-memory-fault",       # the reference touched memory it does not model; pc= op= addr=
    "ref-break",              # a break instruction; pc= op=
    "ref-step-limit",         # the step budget ran out; steps=
    "block-outside-ref-ram",  # a snapshot block the reference cannot hold; addr= size=
    "write-outside-snapshot", # a byte changed outside the image and the blocks; addr= side=
    "aot-no-body",            # the module has no translated body at the entry; pc=
    "aot-dispatch-error",     # the translated entry could not be dispatched; code=
)

_GPR = r"r(?:[1-9]|[12][0-9]|3[01])"
_FPR = r"f(?:[0-9]|[12][0-9]|3[01])"
MISMATCH_FIELD = (
    rf"(?:stop|syscall_pc|syscall_code|import_nid|{_GPR}|hi|lo|{_FPR}|fcr31"
    r"|oor_loads|oor_stores|mem\[0x[0-9a-f]{8}\])"
)
_HEX = r"0x[0-9a-f]+"
_STOP = "(?:" + "|".join(STOP_KINDS) + ")"

VERDICT_RE = re.compile(
    r"^(?:"
    r"(?P<match>MATCH)"
    rf"|MISMATCH (?P<field>{MISMATCH_FIELD}) aot=(?P<aot>{_HEX}|{_STOP}) ref=(?P<ref>{_HEX}|{_STOP})"
    r"|NOT_CHECKED (?P<reason>[a-z][a-z-]*)(?P<details>(?: [a-z_]+=[0-9a-z_x-]+)*)"
    r"|REFUSED (?P<refused>[a-z][a-z-]*)(?: (?P<refused_detail>.+))?"
    r")$"
)


@dataclass(frozen=True)
class Verdict:
    kind: str                 # MATCH, MISMATCH, NOT_CHECKED or REFUSED
    field: str = ""           # MISMATCH: the first differing field
    aot: str = ""
    ref: str = ""
    reason: str = ""          # NOT_CHECKED or REFUSED reason
    details: dict[str, str] = dataclasses.field(default_factory=dict)


def parse_verdict(line: str) -> Verdict:
    """Parse one runner verdict line; raise ValueError when it breaks the grammar."""
    match = VERDICT_RE.match(line.rstrip("\r\n"))
    if match is None:
        raise ValueError(f"not a snapshot verdict line: {line!r}")
    if match.group("match"):
        return Verdict("MATCH")
    if match.group("field"):
        name, aot, ref = match.group("field"), match.group("aot"), match.group("ref")
        stop_values = aot in STOP_KINDS, ref in STOP_KINDS
        if (name == "stop") != stop_values[0] or (name == "stop") != stop_values[1]:
            raise ValueError(f"MISMATCH {name} carries the wrong value kind: {line!r}")
        return Verdict("MISMATCH", field=name, aot=aot, ref=ref)
    if match.group("reason"):
        reason = match.group("reason")
        if reason not in NOT_CHECKED_REASONS:
            raise ValueError(f"unknown NOT_CHECKED reason {reason!r}")
        details = dict(
            item.split("=", 1) for item in match.group("details").split()
        )
        return Verdict("NOT_CHECKED", reason=reason, details=details)
    reason = match.group("refused")
    if reason not in REASONS:
        raise ValueError(f"unknown REFUSED reason {reason!r}")
    return Verdict("REFUSED", reason=reason)
