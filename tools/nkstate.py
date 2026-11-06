# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Read and write ``nkstate`` v1 files: save states (#2146) and replay recordings (#2147).

``src/rt/nkstate.c`` is the runtime's reader and writer for the same format, and
``src/rt/nkstate.h`` documents it. A file binds itself to one build (the CpuState ABI and
size, the executable identity, the runtime build identity and the disc ID) and holds one
segment per subsystem, each with its own version and SHA-256, followed by a SHA-256 of the
whole file. The reader refuses a file with a named reason (``REASONS``), in the same order as
the C reader; ``tools/test_nkstate.py`` pins both to one golden file.

A state holds guest memory and a recording holds a private run's input timing, so neither is
ever committed, uploaded or attached to a report (AGENTS.md section 5).

It also reads replay checkpoints (#2147): ``NKCKPT v=1`` lines, one per checkpoint, written
by ``src/rt/replay_checkpoint.c``. Each holds named SHA-256 digests (``cpu.<uid>``,
``ram.<addr>``, ...) and no guest bytes. ``ckpt-diff`` names the first checkpoint and digest
where two runs split. Lines may sit among other log output; anything else is ignored.

Usage::

    python tools/nkstate.py info FILE     # print the header and segment table, check hashes
    python tools/nkstate.py ckpt-diff A B # first divergent checkpoint between two runs

``ckpt-diff`` prints one line and exits with its status:

    MATCH checkpoints=<n>                                                  0
    MISMATCH frame=<f> index=<k> kind=<frame|names|digest> digest=<name>   1
    NOT_RUN <reason> a=<n> b=<m>                                           3

A run that ends before the other is ``NOT_RUN ended-early`` after its last matching
checkpoint, never a match; a malformed line is ``NOT_RUN malformed``.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"NKSTATE1"
FORMAT_VERSION = 1
KIND_RECORDING = 1
KIND_STATE = 2
FLAG_DETERMINISTIC_CLOCK = 0x1
FLAGS_KNOWN = FLAG_DETERMINISTIC_CLOCK
RUNTIME_ID_LEN = 64
DISC_ID_LEN = 16
MAX_SEGMENTS = 64
HEADER = struct.Struct("<8sIIII32s64s16sII")
ENTRY = struct.Struct("<4sIQ32s")
#: SR_CPUSTATE_ABI_VERSION and sizeof(CpuState) in src/rt/recomp.h (pinned by the test).
ABI_VERSION = 3
CPUSTATE_SIZE = 1000

#: Refusal names, in the order the readers check them (src/rt/nkstate.c s_status_names).
REASONS = (
    "bad-magic",
    "format-version",
    "kind",
    "abi-version",
    "cpustate-size",
    "executable-identity",
    "runtime-identity",
    "disc-id",
    "flags",
    "clock-not-deterministic",
    "truncated",
    "too-many-segments",
    "bad-tag",
    "duplicate-segment",
    "unknown-segment",
    "missing-segment",
    "segment-version",
    "trailing-bytes",
    "segment-hash",
    "file-hash",
)


class StateError(ValueError):
    """A refused file. ``reason`` is one of ``REASONS``."""

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason
        self.detail = detail


@dataclass(frozen=True)
class Identity:
    kind: int
    executable: bytes  # 32 bytes
    runtime: str
    disc_id: str
    flags: int = 0


@dataclass(frozen=True)
class Segment:
    tag: str
    version: int
    payload: bytes


@dataclass
class StateFile:
    identity: Identity
    abi_version: int
    cpustate_size: int
    segments: list[Segment] = field(default_factory=list)


def _fixed(text: str, width: int, what: str) -> bytes:
    raw = text.encode("ascii")
    if len(raw) > width:
        raise ValueError(f"{what} is longer than {width} bytes")
    return raw.ljust(width, b"\0")


def _unfixed(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def _tag_ok(tag: bytes) -> bool:
    return len(tag) == 4 and all(0x20 <= b <= 0x7E for b in tag)


def build(identity: Identity, segments: list[Segment], *, abi_version: int = ABI_VERSION,
          cpustate_size: int = CPUSTATE_SIZE) -> bytes:
    """The file image the C writer produces for the same identity and segments."""
    if not identity.runtime:
        raise StateError("no-runtime-identity", "an ad-hoc build writes no state")
    if len(segments) > MAX_SEGMENTS:
        raise StateError("too-many-segments", f"{len(segments)} segments")
    seen: set[str] = set()
    for seg in segments:
        if not _tag_ok(seg.tag.encode("ascii")):
            raise StateError("bad-tag", repr(seg.tag))
        if seg.tag in seen:
            raise StateError("duplicate-segment", seg.tag)
        seen.add(seg.tag)
    out = bytearray(HEADER.pack(
        MAGIC, FORMAT_VERSION, identity.kind, abi_version, cpustate_size, identity.executable,
        _fixed(identity.runtime, RUNTIME_ID_LEN, "runtime identity"),
        _fixed(identity.disc_id, DISC_ID_LEN, "disc ID"), identity.flags, len(segments)))
    for seg in segments:
        out += ENTRY.pack(seg.tag.encode("ascii"), seg.version, len(seg.payload),
                          hashlib.sha256(seg.payload).digest())
    for seg in segments:
        out += seg.payload
    out += hashlib.sha256(out).digest()
    return bytes(out)


def parse(data: bytes, *, self_identity: Identity | None = None,
          segment_versions: dict[str, int] | None = None,
          require_deterministic_clock: bool = False) -> StateFile:
    """Parse and check ``data``.

    With ``self_identity`` the identity fields must match it, as a load requires. With
    ``segment_versions`` every segment must be one of those tags at that version, and every
    one of those tags must be present. Without them (``info``) only the structure and hashes
    are checked.
    """
    if len(data) < len(MAGIC):
        if data != MAGIC[: len(data)]:
            raise StateError("bad-magic", "not an nkstate file")
        raise StateError("truncated", f"{len(data)} bytes, shorter than the magic")
    if data[:8] != MAGIC:
        raise StateError("bad-magic", "not an nkstate file")
    if len(data) < HEADER.size:
        raise StateError("truncated", f"{len(data)} bytes, shorter than the {HEADER.size}-byte header")
    (_, fmt, kind, abi, cpusize, exe, runtime, disc, flags, count) = HEADER.unpack_from(data, 0)
    ident = Identity(kind, exe, _unfixed(runtime), _unfixed(disc), flags)
    if fmt != FORMAT_VERSION:
        raise StateError("format-version", f"file {fmt}, this reader {FORMAT_VERSION}")
    if self_identity is not None:
        if kind != self_identity.kind:
            raise StateError("kind", f"file kind {kind}, expected {self_identity.kind}")
        if abi != ABI_VERSION:
            raise StateError("abi-version", f"file CpuState ABI {abi}, this build {ABI_VERSION}")
        if cpusize != CPUSTATE_SIZE:
            raise StateError("cpustate-size", f"file {cpusize}, this build {CPUSTATE_SIZE}")
        if exe != self_identity.executable:
            raise StateError("executable-identity", f"file {exe.hex()}, this build {self_identity.executable.hex()}")
        if runtime != _fixed(self_identity.runtime, RUNTIME_ID_LEN, "runtime identity"):
            raise StateError("runtime-identity", f"file '{ident.runtime}', this build '{self_identity.runtime}'")
        if disc != _fixed(self_identity.disc_id, DISC_ID_LEN, "disc ID"):
            raise StateError("disc-id", f"file '{ident.disc_id}', this run '{self_identity.disc_id}'")
    if flags & ~FLAGS_KNOWN:
        raise StateError("flags", f"flags 0x{flags:x} has unknown bits")
    if require_deterministic_clock and not flags & FLAG_DETERMINISTIC_CLOCK:
        raise StateError("clock-not-deterministic", "written without SR_DETERMINISTIC_CLOCK=1")
    if count > MAX_SEGMENTS:
        raise StateError("too-many-segments", f"{count} segments, at most {MAX_SEGMENTS}")
    if len(data) - HEADER.size < count * ENTRY.size:
        raise StateError("truncated", "the segment table runs past the end")
    entries = []
    for i in range(count):
        tag_raw, version, length, digest = ENTRY.unpack_from(data, HEADER.size + i * ENTRY.size)
        if not _tag_ok(tag_raw):
            raise StateError("bad-tag", f"segment {i}")
        tag = tag_raw.decode("ascii")
        if any(tag == e[0] for e in entries):
            raise StateError("duplicate-segment", f"segment '{tag}' appears twice")
        if segment_versions is not None:
            if tag not in segment_versions:
                raise StateError("unknown-segment", f"segment '{tag}': no loader for it")
            if version != segment_versions[tag]:
                raise StateError("segment-version",
                                 f"segment '{tag}' version {version}, this build reads {segment_versions[tag]}")
        entries.append((tag, version, length, digest))
    if segment_versions is not None:
        for tag in segment_versions:
            if all(tag != e[0] for e in entries):
                raise StateError("missing-segment", f"no segment '{tag}'")
    offset = HEADER.size + count * ENTRY.size
    for tag, _, length, _ in entries:
        if length > len(data) - offset:
            raise StateError("truncated", f"segment '{tag}' runs past the end")
        offset += length
    if len(data) - offset < 32:
        raise StateError("truncated", "no room for the file hash")
    if len(data) - offset > 32:
        raise StateError("trailing-bytes", f"{len(data) - offset - 32} bytes after the file hash")
    offset = HEADER.size + count * ENTRY.size
    segments = []
    for tag, version, length, digest in entries:
        payload = data[offset:offset + length]
        if hashlib.sha256(payload).digest() != digest:
            raise StateError("segment-hash", f"segment '{tag}' does not match its hash")
        segments.append(Segment(tag, version, payload))
        offset += length
    if hashlib.sha256(data[:offset]).digest() != data[offset:]:
        raise StateError("file-hash", "the header or segment table was changed")
    return StateFile(ident, abi, cpusize, segments)


def _info(path: Path) -> int:
    try:
        state = parse(path.read_bytes())
    except StateError as exc:
        print(f"REFUSED {exc.reason} {exc.detail}")
        return 2
    ident = state.identity
    kind = {KIND_RECORDING: "recording", KIND_STATE: "state"}.get(ident.kind, f"unknown ({ident.kind})")
    print(f"kind: {kind}")
    print(f"cpustate: ABI {state.abi_version}, {state.cpustate_size} bytes")
    print(f"executable: {ident.executable.hex()}")
    print(f"runtime: {ident.runtime}")
    print(f"disc: {ident.disc_id}")
    print(f"deterministic clock: {'yes' if ident.flags & FLAG_DETERMINISTIC_CLOCK else 'no'}")
    for seg in state.segments:
        print(f"segment '{seg.tag}' v{seg.version}: {len(seg.payload)} bytes")
    return 0


# ---- replay checkpoints --------------------------------------------------------------

CKPT_PREFIX = "NKCKPT v=1 "
CKPT_MAX_DIGESTS = 1024
_CKPT_NAME = re.compile(r"[A-Za-z0-9._-]{1,23}")
_CKPT_HEAD = re.compile(r"NKCKPT v=1 frame=(\d+) n=(\d+)")
_CKPT_DIGEST = re.compile(r" ([^=\s]+)=([0-9a-f]{64})")


@dataclass(frozen=True)
class Checkpoint:
    frame: int
    digests: tuple[tuple[str, str], ...]  # (name, hex) in written order


@dataclass(frozen=True)
class CkptMismatch:
    index: int
    frame: int
    kind: str  # "frame", "names" or "digest"
    name: str


def parse_checkpoint(line: str) -> Checkpoint:
    """Parse one ``NKCKPT v=1`` line, as ``nk_ckpt_parse`` does."""
    line = line.rstrip("\n")
    head = _CKPT_HEAD.match(line)
    if not head:
        raise ValueError("not an NKCKPT v=1 line")
    count = int(head.group(2))
    if count > CKPT_MAX_DIGESTS:
        raise ValueError("too many digests")
    pos = head.end()
    digests = []
    for _ in range(count):
        m = _CKPT_DIGEST.match(line, pos)
        if not m or not _CKPT_NAME.fullmatch(m.group(1)):
            raise ValueError("digest list is short or malformed")
        digests.append((m.group(1), m.group(2)))
        pos = m.end()
    if pos != len(line):
        raise ValueError("trailing text after the digests")
    return Checkpoint(int(head.group(1)), tuple(digests))


def compare_checkpoints(expected: Checkpoint, actual: Checkpoint) -> tuple[str, str] | None:
    """The first difference, in the expected checkpoint's order, as ``nk_ckpt_compare``."""
    if expected.frame != actual.frame:
        return ("frame", "")
    for i in range(max(len(expected.digests), len(actual.digests))):
        if i >= len(expected.digests) or i >= len(actual.digests) or \
                expected.digests[i][0] != actual.digests[i][0]:
            name = expected.digests[i][0] if i < len(expected.digests) else actual.digests[i][0]
            return ("names", name)
        if expected.digests[i][1] != actual.digests[i][1]:
            return ("digest", expected.digests[i][0])
    return None


def read_checkpoints(text: str) -> list[Checkpoint]:
    """Every checkpoint line in ``text``; other lines are ignored, a malformed one raises."""
    out = []
    for line in text.splitlines():
        start = line.find(CKPT_PREFIX)
        if start >= 0:
            out.append(parse_checkpoint(line[start:]))
    return out


def diff_checkpoints(a: list[Checkpoint], b: list[Checkpoint]) -> CkptMismatch | None:
    for i, (ca, cb) in enumerate(zip(a, b, strict=False)):  # a shorter run is NOT_RUN, reported by the caller
        found = compare_checkpoints(ca, cb)
        if found:
            return CkptMismatch(i, ca.frame, found[0], found[1])
    return None


_RUN_HEADER = re.compile(r"(?:NKCKPT_RUN|NKREC) v=1 every=(\d+) clock=(\S+)")


def run_settings(text: str) -> str | None:
    """The run's cadence and clock, ``every=<n> clock=<c>``, from an ``SR_REPLAY_CHECKPOINTS``
    stream's ``NKCKPT_RUN`` line or an ``SR_RECORD`` recording's ``NKREC`` line, or None for a
    bare list of checkpoint lines. A stream and a recording with the same settings compare."""
    for line in text.splitlines():
        m = _RUN_HEADER.search(line)
        if m:
            return f"every={m.group(1)} clock={m.group(2)}"
    return None


def _ckpt_diff(path_a: Path, path_b: Path) -> int:
    text_a = path_a.read_text(encoding="utf-8", errors="replace")
    text_b = path_b.read_text(encoding="utf-8", errors="replace")
    settings_a, settings_b = run_settings(text_a), run_settings(text_b)
    if settings_a != settings_b:
        # Checkpoints taken on another cadence or clock are not comparable.
        print(f"NOT_RUN different-settings a='{settings_a}' b='{settings_b}'")
        return 3
    try:
        a = read_checkpoints(text_a)
        b = read_checkpoints(text_b)
    except ValueError as exc:
        print(f"NOT_RUN malformed {exc}")
        return 3
    mismatch = diff_checkpoints(a, b)
    if mismatch:
        print(f"MISMATCH frame={mismatch.frame} index={mismatch.index} kind={mismatch.kind} "
              f"digest={mismatch.name or '-'}")
        return 1
    if len(a) != len(b) or not a:
        print(f"NOT_RUN {'ended-early' if a or b else 'no-checkpoints'} a={len(a)} b={len(b)}")
        return 3
    print(f"MATCH checkpoints={len(a)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="print the header and segments; check structure and hashes")
    info.add_argument("file", type=Path)
    diff = sub.add_parser("ckpt-diff", help="first divergent replay checkpoint between two runs")
    diff.add_argument("a", type=Path)
    diff.add_argument("b", type=Path)
    args = parser.parse_args(argv)
    if args.command == "info":
        return _info(args.file)
    if args.command == "ckpt-diff":
        return _ckpt_diff(args.a, args.b)
    return 2  # pragma: no cover - argparse requires a command


if __name__ == "__main__":
    sys.exit(main())
