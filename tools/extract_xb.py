# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Batch-extract ClapHanz XB archives with parallel execution, unswizzling, and PNG validation.

Walks a directory tree for .xb / .xb0 / .xb2 / .xb3 files, extracts each
into <output>/<relative-path>.xb.d/ using this repository's own archive
reader, converts Swizzled GIM textures into PNG images, and generates an
inventory map.  No third-party archive library is involved.

Extraction authority
--------------------
This repository owns the whole extraction pipeline:

    repository parser -> repository bounded decoder
                      -> repository member normalization
                      -> repository filesystem writer

``tools/xb_probe.py`` parses the archive and decodes every member, including
the nested ``DEFLATE -> LZS`` layer, under limits declared here; this module
normalizes each member name once and hands the *same* normalized identity to
its own writer.  libxb is no longer a production dependency.

Why the split was removed
-------------------------
An earlier design let this repository validate an archive and then handed the
file to libxb to reinterpret, decompress, and write.  Two things went wrong
with that arrangement, and both are structural rather than incidental:

* **Nested expansion escaped every repository budget.**  Validating names and
  outer archive structure says nothing about a nested compression layer.  An
  entry whose FST expanded size is 1 byte can carry a DEFLATE payload whose
  inner LZS stream declares a megabyte; the name check accepted it and libxb
  decoded and wrote the full expansion.  A byte-producing layer is only bounded
  if the code that produces the bytes is the code that carries the budget.
* **The validated name was not the written name.**  libxb rewrote forward
  slashes to backslashes before writing.  On Windows a backslash is still a
  separator; on POSIX it is an ordinary filename character, so the effective
  path on disk was not the path that had been proven contained.

The fix for both is the same: one parser, one decoder, one path identity.

Path identity
-------------
The XB container's own grammar treats backslash and forward slash alike as
separators, so that is the canonical interpretation *independent of the host
OS* -- an archive must extract to the same tree everywhere.  It is the format
that decides what a separator is, not the machine running the extractor.
``normalize_member_path`` returns
POSIX-separated components and :func:`member_components` splits them; the
writer only ever receives those components.  The original archive string is
never re-derived after validation, which is what makes
``VALIDATED_MEMBER_PATH == WRITTEN_MEMBER_PATH`` an invariant rather than a
hope.

Containment is resolved against the canonical destination root, and the
produced tree is re-verified by :func:`verify_extracted_tree` afterwards.  That
verification is defense in depth only: it can detect a tree that is wrong, it
cannot undo a write that already escaped.  Prevention is the normalization and
root resolution that run before each write.  The bounded whole-file reads use
one open handle for their size check and data, while hostile concurrent
mutation of the destination during the multi-step extraction remains outside
the current threat model.

Resource limits are repository-owned throughout -- see the budget contract
below.  Whole files are never read without a size gate, worker parallelism is
capped rather than inherited from the CPU count, and the number of in-flight
worker tasks is bounded independently of how many archives were found.

Usage:
    python tools/extract_xb.py <xbdata-dir> [--output <out-dir>] [--workers <n>]
"""

import os
import sys
import argparse
import time
import struct
import stat
import tempfile
import zlib
import json
import re
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor
from concurrent.futures import wait as futures_wait

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

from xb_probe import XBArchiveReader, XBLimits, XBProbeError  # noqa: E402

XB_EXTENSIONS = (".xb", ".xb0", ".xb2", ".xb3")

GIM_MAX_DIMENSION = 4096
GIM_MAX_PIXELS = 16 * 1024 * 1024
GIM_MAX_RAW_BYTES = 64 * 1024 * 1024
GIM_MAX_BLOCKS = 1 << 20
GIM_MAX_NESTING = 32

# ---------------------------------------------------------------------------
# Resource-budget contract
#
# These are the only extraction budgets.  They are small, explicit, and chosen
# so their interaction is testable rather than so they are individually
# unreachable.  Each one is checked before the allocation or write it governs.
#
#   MAX_RAW_ARCHIVE_BYTES     bytes read from one .xb file on disk
#   MAX_ENTRY_DECODED_BYTES   fully decoded bytes of any single member, and of
#                             any intermediate produced by a nested layer
#   MAX_ARCHIVE_DECODED_BYTES decoded bytes summed across one archive
#   MAX_GIM_BYTES             bytes read for one GIM decode candidate
#   DEFAULT_MAX_WORKERS       worker processes when --workers is not given
#   EXPLICIT_MAX_WORKERS      ceiling on an explicit --workers value
#   MAX_IN_FLIGHT_TASKS       worker tasks submitted but not yet collected
#
# The nested-expansion bound is not a separate number: xb_probe binds a nested
# LZS stream to the FST expanded size of its entry, and that size is already
# capped by MAX_ENTRY_DECODED_BYTES and accumulated into
# MAX_ARCHIVE_DECODED_BYTES.  Neither the raw archive size, the compressed
# entry size, nor the outer DEFLATE header is ever used as a proxy for how much
# output a recursive decode can produce.
# ---------------------------------------------------------------------------

MAX_RAW_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_ENTRY_DECODED_BYTES = 64 * 1024 * 1024
MAX_ARCHIVE_DECODED_BYTES = 512 * 1024 * 1024

# A GIM is extracted from an archive member, so it cannot legitimately exceed
# the per-entry decode budget.  Tying the two removes a free-floating constant:
# the largest PSP texture this decoder accepts (4096 x 4096 x 4 bytes) is
# exactly 64 MiB, so the budget is not narrower than the decoder's own limits.
MAX_GIM_BYTES = MAX_ENTRY_DECODED_BYTES

# Sniffing a non-".gim" candidate only needs the 11-byte magic; classifying a
# layout only needs the first 128 bytes.  Neither ever reads a whole file.
GIM_MAGIC = b"MIG.00.1PSP"
SNIFF_BYTES = 128

# Each worker holds a whole archive plus its decode buffers in memory, so the
# default is a small fixed cap rather than os.cpu_count().  An explicit
# --workers value is still bounded.
DEFAULT_MAX_WORKERS = 4
EXPLICIT_MAX_WORKERS = 32

# Futures are heavyweight: each pins its argument tuple and its result until it
# is collected.  Submitting one per discovered archive makes peak bookkeeping
# grow with the input tree, so submission is throttled to a small multiple of
# the worker count instead.  This is a queue depth, not a scheduler.
IN_FLIGHT_TASKS_PER_WORKER = 2


def max_in_flight_tasks(workers):
    """Return MAX_IN_FLIGHT_TASKS for a given worker count."""

    return max(1, workers * IN_FLIGHT_TASKS_PER_WORKER)


def extractor_limits():
    """Return the repository-owned limits handed to the archive reader.

    The reader's own defaults are looser than this tool needs.  Passing these
    explicitly is what makes the budget contract above the operative one.
    """

    return XBLimits(
        max_archive_bytes=MAX_RAW_ARCHIVE_BYTES,
        max_entry_bytes=MAX_ENTRY_DECODED_BYTES,
        max_total_expanded_bytes=MAX_ARCHIVE_DECODED_BYTES,
    )


# Containment limits for archive member names.
MAX_MEMBER_PATH_CHARS = 1024
MAX_MEMBER_COMPONENT_CHARS = 255

# Characters that are invalid in a Windows path component.  ':' also covers
# drive-qualified names and NTFS alternate data streams; '?' and '*' also
# appear in the \\?\ and \\.\ device prefixes.  Rejecting the whole set on
# every platform keeps the boundary identical everywhere and fails closed.
_FORBIDDEN_MEMBER_CHARS = frozenset('<>:"|?*')

# Windows reserved device names.  A file called "NUL" or "COM1.txt" does not
# land in the destination directory at all, so containment cannot be asserted
# for it; refuse rather than pretend.
_RESERVED_DEVICE_NAMES = frozenset(
    ["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"]
    + [f"COM{i}" for i in range(10)]
    + [f"LPT{i}" for i in range(10)]
)

_DRIVE_RE = re.compile(r"^[A-Za-z]:")


class UnsafeArchivePathError(ValueError):
    """Raised when an archive member name cannot be contained in the output."""


class OversizedInputError(ValueError):
    """Raised when an input file exceeds its repository-owned size budget."""


def _split_archive_grammar(name):
    """Layer 1 -- the XB container's own path grammar.

    The format stores member names with ``\\`` as its separator and tolerates
    ``/``; both are separators *in the container*, so both are split here on
    every host.  This is deliberately not host-dependent: the same archive must
    produce the same tree on Windows and on POSIX, and letting the host decide
    what counts as a separator is exactly how a validated name and a written
    name come apart.  Characters that no host can represent in a filename --
    NUL and C0/DEL controls -- are rejected at this layer because they are not
    valid in the grammar either.
    """

    if not isinstance(name, str):
        raise UnsafeArchivePathError(f"member name is not a string: {name!r}")
    if not name:
        raise UnsafeArchivePathError("empty member name")
    if len(name) > MAX_MEMBER_PATH_CHARS:
        raise UnsafeArchivePathError("member name exceeds the length budget")
    if "\x00" in name:
        raise UnsafeArchivePathError(f"NUL in member name: {name!r}")
    for char in name:
        if ord(char) < 0x20 or ord(char) == 0x7F:
            raise UnsafeArchivePathError(f"control character in member name: {name!r}")
    return name.replace("\\", "/")


def _check_extractor_policy(normalized, name):
    """Layer 2 -- portable extractor policy, identical on every host.

    Nothing here is about what a particular filesystem tolerates; it is about
    what an extractor is willing to resolve relative to a destination root.
    """

    # Rooted forms.  Once separators are unified this also catches UNC
    # ("//server/share") and the "//?/" and "//./" device prefixes.
    if normalized.startswith("/"):
        raise UnsafeArchivePathError(f"rooted member name: {name!r}")
    if _DRIVE_RE.match(normalized):
        raise UnsafeArchivePathError(f"drive-qualified member name: {name!r}")
    for part in normalized.split("/"):
        if not part:
            raise UnsafeArchivePathError(f"empty path component in member name: {name!r}")
        if part in (".", ".."):
            raise UnsafeArchivePathError(f"traversal component in member name: {name!r}")
        if len(part) > MAX_MEMBER_COMPONENT_CHARS:
            raise UnsafeArchivePathError("path component exceeds the length budget")


def _check_host_portability(normalized, name):
    """Layer 3 -- host filename restrictions, applied uniformly by choice.

    These rules originate on Windows: reserved device names, the characters
    ``<>:"|?*``, and trailing dots or spaces.  They are enforced on POSIX too,
    and that is a deliberate portability decision rather than an oversight --
    an archive that extracts to one tree on Linux and a different (or partial)
    tree on Windows is a correctness problem for a cross-platform project, and
    a name Windows silently rewrites is a name that was validated in one form
    and written in another.  Rejecting the whole archive is the fail-closed
    reading; a host-conditional rule would make containment host-dependent.
    """

    for part in normalized.split("/"):
        if _FORBIDDEN_MEMBER_CHARS.intersection(part):
            raise UnsafeArchivePathError(f"reserved character in member name: {name!r}")
        if part[-1] in (".", " "):
            raise UnsafeArchivePathError(f"trailing dot or space in member name: {name!r}")
        if part.split(".", 1)[0].upper() in _RESERVED_DEVICE_NAMES:
            raise UnsafeArchivePathError(f"reserved device name in member name: {name!r}")


def normalize_member_path(name):
    """Return the canonical relative POSIX member path, or raise.

    This is the single authoritative interpretation of an archive member name.
    It is purely lexical -- callers must still resolve the result against the
    canonical destination root via :func:`contained_path` -- but no other code
    may re-derive a path from the original archive string afterwards.  The
    three layers it applies are kept separate above so a reviewer can see which
    rule comes from the container format, which from extractor policy, and
    which from host filename limits.
    """

    normalized = _split_archive_grammar(name)
    _check_extractor_policy(normalized, name)
    _check_host_portability(normalized, name)
    return normalized


def member_components(rel_posix):
    """Split a normalized member path into the components the writer uses.

    The writer takes these components, never a string it splits again.  That is
    what keeps the written path identical to the validated one on every host,
    including POSIX, where a backslash left in a name would otherwise become an
    ordinary filename character instead of the separator the format intends.
    """

    return rel_posix.split("/")


def _is_reparse_point(path):
    """True for a symlink or a Windows junction/mount point."""

    try:
        st = os.lstat(path)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    attributes = getattr(st, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return bool(reparse_flag and attributes & reparse_flag)


def canonical_root(path):
    """Return the canonical (link-resolved) form of an extraction root."""

    return os.path.realpath(os.path.abspath(path))


def _is_within(root_real, candidate_real):
    if candidate_real == root_real:
        return True
    prefix = root_real if root_real.endswith(os.sep) else root_real + os.sep
    return candidate_real.startswith(prefix)


def contained_path(root_real, rel_posix):
    """Resolve ``rel_posix`` under ``root_real``, failing closed on any escape.

    ``rel_posix`` must already have passed :func:`normalize_member_path`.  Every
    existing directory between the root and the target is checked for a reparse
    point, because a junction planted inside the destination would let a
    lexically clean name land outside the root.
    """

    components = member_components(rel_posix)
    target = os.path.join(root_real, *components)
    partial = root_real
    for component in components:
        partial = os.path.join(partial, component)
        if _is_reparse_point(partial):
            raise UnsafeArchivePathError(
                f"reparse point on the destination path: {partial}"
            )
    resolved = os.path.realpath(target)
    if not _is_within(root_real, resolved):
        raise UnsafeArchivePathError(
            f"member escapes the extraction root: {rel_posix!r} -> {resolved}"
        )
    return target


def safe_join(root, name):
    """Normalize ``name`` and resolve it beneath ``root``; raise on any escape."""

    return contained_path(canonical_root(root), normalize_member_path(name))


def verify_extracted_tree(dest_dir):
    """Re-verify a produced tree; raise :class:`UnsafeArchivePathError` on escape.

    Detection, not prevention -- the preflight is what stops an unsafe write
    from happening at all.  This catches what string normalization cannot
    promise: a reparse point that appeared inside the tree, or an entry whose
    real location is not beneath the canonical root.
    """

    root_real = canonical_root(dest_dir)
    for dirpath, dirnames, filenames in os.walk(dest_dir, followlinks=False):
        for name in list(dirnames) + list(filenames):
            full = os.path.join(dirpath, name)
            if _is_reparse_point(full):
                raise UnsafeArchivePathError(f"reparse point in extracted tree: {full}")
            if not _is_within(root_real, os.path.realpath(full)):
                raise UnsafeArchivePathError(f"extracted path escapes the root: {full}")


def open_archive(archive_path, endian="<", limits=None):
    """Open one archive under repository-owned limits, or refuse it.

    An archive this reader cannot parse is refused rather than passed to some
    other implementation: an unparsed archive is an unvalidated archive.
    """

    try:
        return XBArchiveReader(
            archive_path, endian=endian, limits=limits or extractor_limits()
        )
    except XBProbeError as exc:
        raise UnsafeArchivePathError(
            f"refusing to extract an archive that cannot be validated: {exc}"
        ) from exc


def plan_archive_members(reader, dest_dir):
    """Normalize and contain every member name; return ``[(rel, entry), ...]``.

    The ``rel`` values returned here are the identities the writer uses.  No
    later stage re-reads ``entry.path``.  The plan also rejects collisions in
    the case-insensitive component identity used by Windows filesystems.  A
    complete lowercased path is not enough: ``Foo/a.bin`` and ``foo/b.bin``
    would otherwise be distinct strings while sharing one on-disk directory.
    """

    root_real = canonical_root(dest_dir)
    plan = []
    # Keep a small path trie keyed by case-folded components.  Besides
    # rejecting case-colliding sibling components, the terminal marker catches
    # a member that is both a file and a directory prefix of another member.
    trie = {"children": {}, "terminal": None}
    for entry in reader.entries:
        rel = normalize_member_path(entry.path)
        contained_path(root_real, rel)
        node = trie
        components = member_components(rel)
        for index, component in enumerate(components):
            key = component.casefold()
            child = node["children"].get(key)
            if child is None:
                child = {
                    "name": component,
                    "owner": entry.path,
                    "children": {},
                    "terminal": None,
                }
                node["children"][key] = child
            elif child["name"] != component:
                raise UnsafeArchivePathError(
                    "colliding member path components in archive: "
                    f"{child['owner']!r} and {entry.path!r}"
                )
            if index < len(components) - 1 and child["terminal"] is not None:
                raise UnsafeArchivePathError(
                    "member path collides with an existing file in archive: "
                    f"{child['terminal']!r} and {entry.path!r}"
                )
            node = child
        if node["terminal"] is not None:
            raise UnsafeArchivePathError(
                f"colliding member names in archive: {node['terminal']!r} and "
                f"{entry.path!r}"
            )
        if node["children"]:
            child_owner = next(iter(node["children"].values()))["owner"]
            raise UnsafeArchivePathError(
                "member path collides with a directory in archive: "
                f"{entry.path!r} and {child_owner!r}"
            )
        node["terminal"] = entry.path
        plan.append((rel, entry))
    return plan


def preflight_archive_members(archive_path, dest_dir, endian="<", limits=None):
    """Validate names *and* decode budgets without writing anything.

    Retained as the standalone check used by tests and by callers that want a
    verdict without a destination tree.  Decoding here is what closes the gap a
    name-only preflight leaves: a nested ``DEFLATE -> LZS`` layer produces bytes,
    so it has to be produced under the budget to be bounded by it.
    """

    reader = open_archive(archive_path, endian=endian, limits=limits)
    plan = plan_archive_members(reader, dest_dir)
    _decode_within_budget(reader, plan, sink=None)
    return [rel for rel, _ in plan]


def _decode_within_budget(reader, plan, sink):
    """Decode each planned member under the per-entry and archive budgets.

    ``sink`` is called as ``sink(rel, data)`` for each member, or ``None`` to
    decode for validation only.  Returns the total decoded byte count.

    The per-entry limit is enforced by the reader (it is passed down as
    ``XBLimits.max_entry_bytes`` and bounds the nested layers too); it is
    re-asserted here so this function's own contract does not depend on how the
    reader was constructed.  The running total is the aggregate bound, and it
    is checked before the next member is decoded rather than afterwards.
    """

    decoded_total = 0
    for rel, entry in plan:
        if entry.expanded_size > MAX_ENTRY_DECODED_BYTES:
            raise UnsafeArchivePathError(
                f"member {rel!r} declares {entry.expanded_size} decoded bytes, "
                f"over the {MAX_ENTRY_DECODED_BYTES}-byte per-entry budget"
            )
        if decoded_total + entry.expanded_size > MAX_ARCHIVE_DECODED_BYTES:
            raise UnsafeArchivePathError(
                f"archive exceeds the {MAX_ARCHIVE_DECODED_BYTES}-byte "
                f"aggregate decode budget at member {rel!r}"
            )
        try:
            data = reader.read_entry(entry)
        except XBProbeError as exc:
            # This is where a nested DEFLATE/LZS layer that disagrees with its
            # FST size is rejected -- the decoder binds the inner stream to the
            # declared entry size, so an inner claim of a larger expansion
            # cannot produce output at all.
            raise UnsafeArchivePathError(
                f"refusing member {rel!r}: {exc}"
            ) from exc
        if len(data) > MAX_ENTRY_DECODED_BYTES:
            raise UnsafeArchivePathError(
                f"member {rel!r} decoded to {len(data)} bytes, over the "
                f"{MAX_ENTRY_DECODED_BYTES}-byte per-entry budget"
            )
        decoded_total += len(data)
        if decoded_total > MAX_ARCHIVE_DECODED_BYTES:
            raise UnsafeArchivePathError(
                f"archive decoded to more than {MAX_ARCHIVE_DECODED_BYTES} bytes"
            )
        if sink is not None:
            sink(rel, data)
    return decoded_total


def _make_parents_within(root_real, rel_posix):
    """Create the directories for ``rel_posix`` beneath ``root_real`` only.

    Each level is re-resolved as it is created, so a directory that would land
    outside the root -- or a reparse point that appeared partway through -- is
    refused at the level it appears rather than after the fact.
    """

    components = member_components(rel_posix)
    partial = ""
    for component in components[:-1]:
        partial = f"{partial}/{component}" if partial else component
        directory = contained_path(root_real, partial)
        os.makedirs(directory, exist_ok=True)


def write_member(root_real, rel_posix, data, overwrite=False):
    """Write one decoded member at its validated identity.

    ``rel_posix`` is the value :func:`normalize_member_path` produced and
    :func:`contained_path` accepted.  It is resolved once more here so the
    check and the write see the same path.
    """

    _make_parents_within(root_real, rel_posix)
    target = contained_path(root_real, rel_posix)
    _write_exclusive(target, data, overwrite=overwrite)
    return target


def extract_archive(archive_path, dest_dir, overwrite=False, endian="<", limits=None):
    """Parse, decode, and write one archive entirely with repository-owned code.

    Returns ``(written_relative_paths, decoded_bytes)``.  Names are validated
    and contained before any decode begins, and each member is decoded under
    the budget contract immediately before it is written at the identity that
    was validated.
    """

    root_real = canonical_root(dest_dir)
    reader = open_archive(archive_path, endian=endian, limits=limits)
    plan = plan_archive_members(reader, root_real)

    written = []

    def sink(rel, data):
        write_member(root_real, rel, data, overwrite=overwrite)
        written.append(rel)

    decoded_total = _decode_within_budget(reader, plan, sink)
    return written, decoded_total


def read_bounded(path, max_bytes):
    """Read a whole file through one handle under ``max_bytes``.

    ``fstat`` and the bounded read operate on the same open handle, so a
    replacement or growth race cannot make a different file bypass the size
    gate.  The extra byte detects growth while the handle is being read.
    """

    if max_bytes < 0:
        raise ValueError("max_bytes must be non-negative")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        size = os.fstat(fd).st_size
        if size > max_bytes:
            raise OversizedInputError(
                f"{path} is {size} bytes, over the {max_bytes}-byte budget"
            )
        chunks = []
        remaining = max_bytes + 1
        while remaining:
            chunk = os.read(fd, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
    finally:
        os.close(fd)
    if len(data) > max_bytes:
        raise OversizedInputError(f"{path} grew past the {max_bytes}-byte budget")
    return data


def _write_exclusive(path, payload, overwrite=False):
    """Write ``payload`` without clobbering an existing file unless asked to.

    ``O_EXCL`` is what makes the default no-overwrite; it also refuses to follow
    a symlink planted at ``path``, so the check and the write cannot disagree.
    ``os.write`` is allowed to write fewer bytes than it was given, so the loop
    below is not defensive padding: a member here can be tens of megabytes, and
    a short write would truncate it silently.
    """

    flags = os.O_WRONLY | os.O_CREAT | getattr(os, "O_BINARY", 0)
    flags |= os.O_TRUNC if overwrite else os.O_EXCL
    fd = os.open(path, flags, 0o644)
    try:
        view = memoryview(payload)
        while view:
            n = os.write(fd, view)
            if n <= 0:
                raise OSError(f"write made no progress: wrote {n} bytes to {path}")
            if n > len(view):
                raise OSError(
                    f"write returned count {n} larger than remaining {len(view)} bytes to {path}"
                )
            view = view[n:]
    finally:
        os.close(fd)


def unswizzle(src, pitch, height):
    """PSP GE texture unswizzle: 16-byte x 8-line tiles."""
    if pitch % 16 != 0 or height % 8 != 0:
        return src  # Can't unswizzle non-aligned
    dst = bytearray(len(src))
    rowblocks = pitch // 16
    for y in range(height):
        for x in range(pitch):
            bx, by = x // 16, y // 8
            px, py = x % 16, y % 8
            block_idx = bx + by * rowblocks
            src_off = block_idx * 128 + py * 16 + px
            dst_off = y * pitch + x
            if src_off < len(src) and dst_off < len(dst):
                dst[dst_off] = src[src_off]
    return bytes(dst)


def decode_gim_data(data):
    """Decode GIM data to RGBA pixels (width, height, pixels_bytes)."""
    if not isinstance(data, (bytes, bytearray, memoryview)):
        return None
    if len(data) < 16 or data[0:11] != b'MIG.00.1PSP':
        return None

    image_block = None
    palette_data = None
    block_count = 0
    malformed = False

    def checked_range(start, size, limit):
        if start < 0 or size < 0 or start > limit or size > limit - start:
            return None
        return start + size

    def scan_blocks(offset, limit, depth=0):
        nonlocal palette_data, image_block, block_count, malformed
        if depth > GIM_MAX_NESTING or offset < 0 or limit < offset or limit > len(data):
            malformed = True
            return False
        while offset < limit:
            if limit - offset < 16:
                malformed = True
                return False
            block_id = struct.unpack_from('<H', data, offset)[0]
            block_size = struct.unpack_from('<I', data, offset + 4)[0]
            hdr_size = struct.unpack_from('<I', data, offset + 12)[0]
            block_end = checked_range(offset, block_size, limit)
            if block_size < 16 or hdr_size < 16 or hdr_size > block_size or block_end is None:
                malformed = True
                return False
            content = offset + hdr_size
            content_limit = block_end
            block_count += 1
            if block_count > GIM_MAX_BLOCKS:
                malformed = True
                return False
            if block_id == 0x0004:
                if content_limit - content < 36:
                    malformed = True
                    return False
                fmt = struct.unpack_from('<H', data, content + 4)[0]
                swiz = struct.unpack_from('<H', data, content + 6)[0]
                w = struct.unpack_from('<H', data, content + 8)[0]
                h = struct.unpack_from('<H', data, content + 10)[0]
                d_off = struct.unpack_from('<I', data, content + 28)[0]
                d_end = struct.unpack_from('<I', data, content + 32)[0]
                raw_len = d_end - d_off if d_end >= d_off else -1
                if (w == 0 or h == 0 or w > GIM_MAX_DIMENSION or h > GIM_MAX_DIMENSION or
                    w * h > GIM_MAX_PIXELS or raw_len < 0 or raw_len > GIM_MAX_RAW_BYTES or
                    d_end > content_limit - content):
                    malformed = True
                    return False
                if image_block is None:
                    image_block = (fmt, swiz, w, h, data[content + d_off:content + d_end])
            elif block_id == 0x0005:
                if content_limit - content < 36:
                    malformed = True
                    return False
                p_off = struct.unpack_from('<I', data, content + 28)[0]
                p_end = struct.unpack_from('<I', data, content + 32)[0]
                palette_len = p_end - p_off if p_end >= p_off else -1
                if palette_len < 0 or palette_len > GIM_MAX_RAW_BYTES or p_end > content_limit - content:
                    malformed = True
                    return False
                palette_data = data[content + p_off:content + p_end]
            elif block_id in (0x0002, 0x0003):
                if not scan_blocks(content, content_limit, depth + 1):
                    return False
            offset = block_end
        return True

    if not scan_blocks(16, len(data)) or malformed or image_block is None:
        return None

    fmt, swiz, w, h, pix_data = image_block

    # Build palette for indexed formats
    clut = None
    if palette_data and len(palette_data) >= 4:
        num_entries = len(palette_data) // 4
        clut = []
        for i in range(min(num_entries, 256)):
            val = struct.unpack_from('<I', palette_data, i * 4)[0]
            clut.append(val)

    # Determine bytes per pixel element
    bpp_map = {0: 2, 1: 2, 2: 2, 3: 4, 4: 0, 5: 1}
    bpp = bpp_map.get(fmt, 4)

    if fmt in (0, 1, 2, 3):
        pitch = w * bpp
    elif fmt == 4:
        pitch = (w + 1) // 2
    elif fmt == 5:
        pitch = w
    else:
        return None

    raw_size = pitch * h
    pixel_size = w * h * 4
    if (pitch <= 0 or raw_size > GIM_MAX_RAW_BYTES or len(pix_data) < raw_size or
        pixel_size > GIM_MAX_RAW_BYTES):
        return None

    # Unswizzle if needed
    if swiz == 1 and pitch > 0 and h > 0:
        pix_data = unswizzle(pix_data[:raw_size], pitch, h)

    # Convert to RGBA8888
    pixels = bytearray(pixel_size)

    if fmt == 3:  # RGBA8888 direct (or BGRA, depending on layout, we output RGBA for PNG)
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 4
                di = (y * w + x) * 4
                if si + 4 <= len(pix_data) and di + 4 <= len(pixels):
                    # GIM stores BGRA usually, let's map to RGBA
                    pixels[di] = pix_data[si+2] # R
                    pixels[di+1] = pix_data[si+1] # G
                    pixels[di+2] = pix_data[si] # B
                    pixels[di+3] = pix_data[si+3] # A

    elif fmt == 5:  # T8 indexed
        if clut:
            for y in range(h):
                for x in range(w):
                    si = y * pitch + x
                    di = (y * w + x) * 4
                    if si < len(pix_data) and di + 4 <= len(pixels):
                        idx = pix_data[si]
                        if idx < len(clut):
                            val = clut[idx]
                            # Clut is BGRA, swap to RGBA
                            pixels[di] = (val >> 16) & 0xFF # R
                            pixels[di+1] = (val >> 8) & 0xFF # G
                            pixels[di+2] = val & 0xFF # B
                            pixels[di+3] = (val >> 24) & 0xFF # A

    elif fmt == 4:  # T4 indexed
        if clut:
            for y in range(h):
                for x in range(w):
                    si = y * pitch + x // 2
                    di = (y * w + x) * 4
                    if si < len(pix_data) and di + 4 <= len(pixels):
                        byte = pix_data[si]
                        idx = (byte & 0x0F) if (x % 2 == 0) else (byte >> 4)
                        if idx < len(clut):
                            val = clut[idx]
                            pixels[di] = (val >> 16) & 0xFF # R
                            pixels[di+1] = (val >> 8) & 0xFF # G
                            pixels[di+2] = val & 0xFF # B
                            pixels[di+3] = (val >> 24) & 0xFF # A

    elif fmt == 0:  # BGR5650
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 2
                di = (y * w + x) * 4
                if si + 2 <= len(pix_data) and di + 4 <= len(pixels):
                    val = struct.unpack_from('<H', pix_data, si)[0]
                    r = (val & 0x1F); r = (r << 3) | (r >> 2)
                    g = (val >> 5) & 0x3F; g = (g << 2) | (g >> 4)
                    b = (val >> 11) & 0x1F; b = (b << 3) | (b >> 2)
                    pixels[di] = r; pixels[di+1] = g; pixels[di+2] = b; pixels[di+3] = 0xFF

    elif fmt == 1:  # ABGR5551
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 2
                di = (y * w + x) * 4
                if si + 2 <= len(pix_data) and di + 4 <= len(pixels):
                    val = struct.unpack_from('<H', pix_data, si)[0]
                    r = (val & 0x1F); r = (r << 3) | (r >> 2)
                    g = (val >> 5) & 0x1F; g = (g << 3) | (g >> 2)
                    b = (val >> 10) & 0x1F; b = (b << 3) | (b >> 2)
                    a = 0xFF if (val & 0x8000) else 0x00
                    pixels[di] = r; pixels[di+1] = g; pixels[di+2] = b; pixels[di+3] = a

    elif fmt == 2:  # ABGR4444
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 2
                di = (y * w + x) * 4
                if si + 2 <= len(pix_data) and di + 4 <= len(pixels):
                    val = struct.unpack_from('<H', pix_data, si)[0]
                    r = val & 0x0F; r |= r << 4
                    g = (val >> 4) & 0x0F; g |= g << 4
                    b = (val >> 8) & 0x0F; b |= b << 4
                    a = (val >> 12) & 0x0F; a |= a << 4
                    pixels[di] = r; pixels[di+1] = g; pixels[di+2] = b; pixels[di+3] = a

    return (w, h, bytes(pixels))


def save_png(w, h, rgba_pixels, out_path, overwrite=False):
    """Write standard PNG from raw RGBA pixels without any external library.

    Refuses to clobber an existing file unless ``overwrite`` is set, so a
    generated ``<name>.png`` cannot silently replace an extracted member that
    already carries that name.
    """
    raw = b"".join(b"\x00" + rgba_pixels[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) # Color type 6 = RGBA
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))

    _write_exclusive(out_path, png, overwrite=overwrite)


def process_extracted_directory(dest_dir, overwrite=False, max_gim_bytes=MAX_GIM_BYTES):
    """Scan extracted directory to classify files and convert GIMs to PNGs.

    ``max_gim_bytes`` bounds every whole-file read: a candidate is rejected from
    its size before the decoder sees it, so an oversized file is never paged in.
    """
    inventory = {
        "textures": [],
        "sounds": [],
        "scene_graphs": [],
        "other": [],
        "rejected": []
    }

    for dirpath, _, filenames in os.walk(dest_dir, followlinks=False):
        for fn in filenames:
            fpath = os.path.join(dirpath, fn)
            rel_fpath = os.path.relpath(fpath, dest_dir)
            if _is_reparse_point(fpath):
                inventory["rejected"].append({
                    "name": fn,
                    "path": rel_fpath,
                    "reason": "reparse point"
                })
                continue
            sz = os.path.getsize(fpath)

            # Skip generated pngs themselves
            if fn.lower().endswith(".png"):
                continue

            is_gim = fn.lower().endswith(".gim")
            if not is_gim:
                try:
                    with open(fpath, "rb") as f:
                        magic = f.read(len(GIM_MAGIC))
                        if magic == GIM_MAGIC:
                            is_gim = True
                except Exception:
                    pass

            if is_gim:
                png_fn = os.path.splitext(fn)[0] + ".png"
                png_path = os.path.join(dirpath, png_fn)
                rel_png_path = os.path.relpath(png_path, dest_dir)
                try:
                    # read_bounded checks the size on the same handle used for
                    # the bounded read, so an oversized candidate never reaches
                    # the decoder and its bytes are never paged in.
                    gim_data = read_bounded(fpath, max_gim_bytes)
                    decoded = decode_gim_data(gim_data)
                    if decoded:
                        w, h, rgba = decoded
                        save_png(w, h, rgba, png_path, overwrite=overwrite)
                        inventory["textures"].append({
                            "name": fn,
                            "path": rel_fpath,
                            "png_path": rel_png_path,
                            "width": w,
                            "height": h,
                            "size_bytes": sz
                        })
                        continue
                except OversizedInputError:
                    inventory["rejected"].append({
                        "name": fn,
                        "path": rel_fpath,
                        "size_bytes": sz,
                        "reason": "over the GIM input budget"
                    })
                    continue
                except FileExistsError:
                    inventory["rejected"].append({
                        "name": fn,
                        "path": rel_fpath,
                        "size_bytes": sz,
                        "reason": f"{rel_png_path} already exists"
                    })
                    continue
                except Exception:
                    pass

            # Check sounds
            if fn.lower().endswith((".sgd", ".sgb", ".vag", ".at3", ".wav")):
                inventory["sounds"].append({
                    "name": fn,
                    "path": rel_fpath,
                    "size_bytes": sz
                })
                continue

            # Check layouts/scene-graphs
            is_layout = fn.lower().endswith((".xb0", ".xb1", ".xb2", ".xb3", ".dec"))
            if not is_layout:
                try:
                    with open(fpath, "rb") as f:
                        header = f.read(SNIFF_BYTES)
                        if b"MAP1" in header or b"LAY1" in header:
                            is_layout = True
                except Exception:
                    pass

            if is_layout:
                inventory["scene_graphs"].append({
                    "name": fn,
                    "path": rel_fpath,
                    "size_bytes": sz
                })
                continue

            # Other files
            inventory["other"].append({
                "name": fn,
                "path": rel_fpath,
                "size_bytes": sz
            })

    # Write inventory map.  Exclusive by default so an extracted member named
    # inventory_map.json is a loud failure rather than a silent replacement.
    inv_path = os.path.join(dest_dir, "inventory_map.json")
    _write_exclusive(
        inv_path,
        json.dumps(inventory, indent=4).encode("utf-8"),
        overwrite=overwrite,
    )
    return inventory


def resolve_workers(requested):
    """Return a bounded worker count, or ``None`` if ``requested`` is invalid.

    The default is capped at :data:`DEFAULT_MAX_WORKERS` rather than inheriting
    ``os.cpu_count()``: every worker holds a whole archive plus its decode
    buffers, so a high-core machine would otherwise multiply peak memory by its
    core count.  An explicit override stays available but is bounded.
    """

    if requested is None:
        return max(1, min(DEFAULT_MAX_WORKERS, os.cpu_count() or 1))
    if not isinstance(requested, int) or isinstance(requested, bool):
        return None
    if requested < 1 or requested > EXPLICIT_MAX_WORKERS:
        return None
    return requested


def submit_bounded(executor, fn, arg_tuples, max_in_flight):
    """Yield ``fn(*args)`` results, never holding more than ``max_in_flight``.

    ``arg_tuples`` may be any iterable, including one much longer than the
    queue depth.  Submission is refilled only as results are collected, so the
    number of live futures -- and therefore the pinned arguments and results --
    is bounded by the worker pool rather than by how many archives were found.

    Results arrive in completion order, which is why every caller prints the
    archive path with its own outcome instead of relying on position.
    """

    if max_in_flight < 1:
        raise ValueError("max_in_flight must be at least 1")
    pending = set()
    source = iter(arg_tuples)
    exhausted = False
    while True:
        while not exhausted and len(pending) < max_in_flight:
            try:
                args = next(source)
            except StopIteration:
                exhausted = True
                break
            pending.add(executor.submit(fn, *args))
        if not pending:
            return
        done, pending = futures_wait(pending, return_when=FIRST_COMPLETED)
        for future in done:
            yield future.result()


def find_xb_files(root):
    """Recursively find all XB archive files under root."""
    matches = []
    for dirpath, _, filenames in os.walk(root):
        for fn in sorted(filenames):
            if fn.lower().endswith(XB_EXTENSIONS):
                matches.append(os.path.join(dirpath, fn))
    return matches


def prepare_destination(out_dir, overwrite=False):
    """Resolve the canonical extraction root, refusing to reuse a populated one.

    Only the *parent* is created here.  The destination itself is created by
    promoting a staging tree, so an archive that is refused leaves no empty
    directory behind.  A destination that is itself a reparse point is refused,
    because its canonical location is not the one that was requested.
    """

    if os.path.lexists(out_dir) and _is_reparse_point(out_dir):
        raise UnsafeArchivePathError(f"destination is a reparse point: {out_dir}")
    if os.path.isdir(out_dir) and os.listdir(out_dir) and not overwrite:
        raise FileExistsError(
            f"{out_dir} already exists and is not empty; pass --overwrite to reuse it"
        )
    parent = os.path.dirname(os.path.abspath(out_dir))
    if parent:
        os.makedirs(parent, exist_ok=True)
    return canonical_root(out_dir)


def _discard_staging(staging):
    """Remove a staging tree this call created, or leave it and say so.

    Only a tree that :func:`verify_extracted_tree` accepts is removed, so the
    deletion never walks through a reparse point.  If verification fails the
    tree is left in place and its path is reported: leaving a directory behind
    is a smaller problem than deleting through something unexpected.
    """

    try:
        verify_extracted_tree(staging)
    except UnsafeArchivePathError:
        return False
    try:
        for dirpath, dirnames, filenames in os.walk(staging, topdown=False):
            for name in filenames:
                os.unlink(os.path.join(dirpath, name))
            for name in dirnames:
                os.rmdir(os.path.join(dirpath, name))
        os.rmdir(staging)
    except OSError:
        return False
    return True


def _promote_staging(staging, dest_real):
    """Move a fully verified staging tree onto the destination.

    The fast path is a plain rename onto a missing destination, which is what a
    normal run produces.  An existing destination—including an empty one—is
    merged entry by entry under ``--overwrite`` so the promotion cannot be used
    to remove anything the caller did not opt into replacing.  The complete
    merge is preflighted before the first replacement, and replaced files are
    kept in the staging tree until the merge succeeds so an unexpected
    filesystem error can be rolled back without leaving a hybrid destination.
    Once every replacement succeeds, promotion is committed; staging cleanup
    is best effort and never rolls back the committed tree.
    """

    if _is_reparse_point(staging):
        raise UnsafeArchivePathError(f"staging is a reparse point: {staging}")
    if os.path.lexists(dest_real) and _is_reparse_point(dest_real):
        raise UnsafeArchivePathError(f"destination is a reparse point: {dest_real}")
    verify_extracted_tree(staging)
    if not os.path.lexists(dest_real):
        os.rename(staging, dest_real)
        return True

    root_real = canonical_root(dest_real)
    if not os.path.isdir(dest_real):
        raise UnsafeArchivePathError(
            f"destination is not a real directory: {dest_real}"
        )

    directories = []
    files = []
    for dirpath, dirnames, filenames in os.walk(
        staging, topdown=True, followlinks=False
    ):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, staging)
        for name in dirnames:
            rel = name if rel_dir == "." else f"{rel_dir}{os.sep}{name}"
            directories.append(rel.replace(os.sep, "/"))
        for name in filenames:
            rel = name if rel_dir == "." else f"{rel_dir}{os.sep}{name}"
            files.append(rel.replace(os.sep, "/"))

    directories.sort(key=lambda rel: (rel.count("/"), rel))
    files.sort()
    for rel_posix in directories:
        target = contained_path(root_real, rel_posix)
        if os.path.lexists(target) and (
            _is_reparse_point(target) or not os.path.isdir(target)
        ):
            raise UnsafeArchivePathError(
                f"cannot promote directory over existing non-directory: {target}"
            )
    for rel_posix in files:
        target = contained_path(root_real, rel_posix)
        if os.path.lexists(target) and (
            _is_reparse_point(target) or not os.path.isfile(target)
        ):
            raise UnsafeArchivePathError(
                f"cannot promote file over existing non-file: {target}"
            )

    actions = []
    created_dirs = []
    backup_root = None
    try:
        for rel_posix in directories:
            target = contained_path(root_real, rel_posix)
            if not os.path.lexists(target):
                os.makedirs(target)
                created_dirs.append(target)

        for rel_posix in files:
            stage_path = os.path.join(staging, *rel_posix.split("/"))
            target = contained_path(root_real, rel_posix)
            backup_path = None
            if os.path.lexists(target):
                if backup_root is None:
                    backup_root = tempfile.mkdtemp(
                        prefix=".promotion-backup-", dir=staging
                    )
                backup_path = os.path.join(backup_root, *rel_posix.split("/"))
                os.makedirs(os.path.dirname(backup_path), exist_ok=True)
                os.replace(target, backup_path)
            action = {
                "target": target,
                "backup": backup_path,
                "promoted": False,
            }
            actions.append(action)
            os.replace(stage_path, target)
            action["promoted"] = True

    except Exception as exc:
        rollback_errors = []
        for action in reversed(actions):
            target = action["target"]
            backup_path = action["backup"]
            try:
                if action["promoted"]:
                    if os.path.lexists(target):
                        if _is_reparse_point(target):
                            raise UnsafeArchivePathError(
                                f"promoted target became a reparse point: {target}"
                            )
                        os.unlink(target)
                if backup_path is not None and os.path.lexists(backup_path):
                    if os.path.lexists(target):
                        raise OSError(
                            f"rollback target is unexpectedly occupied: {target}"
                        )
                    os.replace(backup_path, target)
            except Exception as rollback_error:
                rollback_errors.append(str(rollback_error))
        for directory in reversed(created_dirs):
            try:
                if os.path.isdir(directory) and not _is_reparse_point(directory):
                    os.rmdir(directory)
            except OSError as rollback_error:
                rollback_errors.append(str(rollback_error))
        if rollback_errors:
            detail = "; ".join(rollback_errors)
            raise OSError(f"promotion failed and rollback failed: {detail}") from exc
        raise

    # Commit point: every staged file has reached its validated destination,
    # and all old files remain available in the staging backup until this point.
    # Cleanup is now post-commit.  A partial unlink/rmdir must leave the new
    # destination intact; the leftover staging path is safer evidence than
    # deleting a promoted file and losing the old one during rollback.
    try:
        return _discard_staging(staging)
    except Exception:
        return False


def extract_one(archive_path, out_dir, verbose=False, overwrite=False):
    """Extract a single XB archive. Returns (archive_path, status, error_msg).

    ``status`` is "ok", "skipped", or "failed".  The whole pipeline is
    repository-owned -- parse, decode, normalize, write -- and the archive is
    built in a staging directory beside the destination so a member that
    violates a budget or a containment rule leaves no promoted output at all.
    ``verify_extracted_tree`` then runs on the staging tree as defense in
    depth before it is moved into place.
    """

    try:
        dest_real = prepare_destination(out_dir, overwrite=overwrite)
    except FileExistsError as e:
        return archive_path, "skipped", str(e)
    except Exception as e:
        return archive_path, "failed", str(e)

    staging = dest_real + ".staging"
    try:
        if os.path.lexists(staging):
            raise FileExistsError(
                f"staging directory {staging} already exists; remove it and retry"
            )
        os.makedirs(staging)
    except Exception as e:
        return archive_path, "failed", str(e)

    try:
        written, decoded = extract_archive(
            archive_path, staging, overwrite=False
        )
        verify_extracted_tree(staging)
        process_extracted_directory(staging, overwrite=False)
        verify_extracted_tree(staging)
        cleanup_complete = _promote_staging(staging, dest_real)
        if verbose:
            print(f"  {archive_path}: {len(written)} members, {decoded} decoded bytes")
            if not cleanup_complete:
                print(f"  promotion cleanup deferred; staging remains at {staging}")
        return archive_path, "ok", None
    except Exception as e:
        detail = str(e)
        if not _discard_staging(staging):
            detail = f"{detail} (staging tree left at {staging})"
        return archive_path, "failed", detail


def main():
    parser = argparse.ArgumentParser(
        description="Batch-extract ClapHanz XB archives with multiprocessing and validation"
    )
    parser.add_argument(
        "xbdata_dir",
        help="Root directory to scan for .xb/.xb0/.xb2/.xb3 files",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output directory (default: <xbdata-dir>_extracted)",
    )
    parser.add_argument(
        "-w", "--workers",
        type=int,
        default=None,
        help=(
            f"Number of parallel worker processes "
            f"(default: min(cpu_count, {DEFAULT_MAX_WORKERS}); max {EXPLICIT_MAX_WORKERS})"
        ),
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Reuse a non-empty destination and replace existing generated files",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print each file as it is extracted",
    )
    args = parser.parse_args()

    xbdata_dir = os.path.abspath(args.xbdata_dir)
    if not os.path.isdir(xbdata_dir):
        sys.stderr.write(f"error: {xbdata_dir} is not a directory\n")
        return 1

    max_workers = resolve_workers(args.workers)
    if max_workers is None:
        sys.stderr.write(
            f"error: --workers must be between 1 and {EXPLICIT_MAX_WORKERS}\n"
        )
        return 1

    out_dir = os.path.abspath(args.output) if args.output else xbdata_dir + "_extracted"

    files = find_xb_files(xbdata_dir)
    if not files:
        print(f"No XB archives found under {xbdata_dir}")
        return 0

    print(f"Found {len(files)} XB archives under {xbdata_dir}")
    print(f"Extracting and validating to {out_dir}")

    ok_count = 0
    fail_count = 0
    skip_count = 0
    t0 = time.time()

    # Map each archive path to its destination directory
    tasks = []
    for fpath in files:
        rel = os.path.relpath(fpath, xbdata_dir)
        # Preserve the complete archive suffix.  `.xb0`, `.xb2`, and `.xb3` are
        # localized variants with the same internal paths; collapsing all of them to
        # `<name>.xb.d` made parallel extraction overwrite languages nondeterministically.
        dest = os.path.join(out_dir, rel + ".d")
        tasks.append((fpath, dest))

    # Parallel execution using ProcessPoolExecutor
    in_flight = max_in_flight_tasks(max_workers)
    print(
        f"Starting ProcessPoolExecutor with {max_workers} workers "
        f"({in_flight} tasks in flight)..."
    )
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        results = submit_bounded(
            executor,
            extract_one,
            [(fpath, dest, args.verbose, args.overwrite) for fpath, dest in tasks],
            in_flight,
        )

        for i, (fpath, status, err) in enumerate(results, 1):
            rel = os.path.relpath(fpath, xbdata_dir)
            if status == "ok":
                ok_count += 1
                if not args.verbose:
                    print(f"  [{i}/{len(files)}] OK  {rel}")
            elif status == "skipped":
                skip_count += 1
                print(f"  [{i}/{len(files)}] SKIP {rel}: {err}")
            else:
                fail_count += 1
                print(f"  [{i}/{len(files)}] FAIL {rel}: {err}")

    elapsed = time.time() - t0
    print(
        f"\nDone: {ok_count} extracted, {skip_count} skipped, "
        f"{fail_count} failed ({elapsed:.1f}s)"
    )
    return 1 if fail_count else 0


if __name__ == "__main__":
    sys.exit(main())
