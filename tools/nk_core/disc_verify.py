# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Disc verification: a whole-image hash compared with the user's Redump list.

A disc image is hashed whole (SHA-1 and CRC32, the two hashes Redump lists) on the user's
machine. A compressed image (.cso, .zso, .chd) is hashed after the one-time expansion
(``disc_expand_tool.ensure_plain_iso``), so it hashes the same as the .iso it holds. The
hashes are compared with a Redump datafile the user imports themselves; nothing is uploaded
or downloaded.

Each image gets one tier:

- ``VERIFIED_RETAIL``: its SHA-1 matches a disc in the list, and the CRC32 and size agree.
- ``MODIFIED``: no match, but its disc ID is a known retail disc ID: one the list names (a
  datafile with ``<serial>`` entries), or the disc ID of a verified image in the same
  library. Fan translations, patched and trimmed images land here.
- ``UNKNOWN``: neither.
- ``NOT_CHECKED``: no list was imported, or the image was not hashed.

The datafile is read by a small bounded scanner, the same rules as ``src/core/nk_disc_verify.c``
(no XML entity or DTD processing): each ``<game>`` element, its ``<rom>`` elements' ``size``,
``crc`` and ``sha1`` attributes, and any ``<serial>`` text. A rom with a malformed hash or
size is skipped and counted.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import os
from pathlib import Path
import re
import tempfile
from typing import Callable, Iterable, Mapping, Optional
import zlib

VERIFIED_RETAIL = "VERIFIED_RETAIL"
MODIFIED = "MODIFIED"
UNKNOWN = "UNKNOWN"
NOT_CHECKED = "NOT_CHECKED"
TIERS = (VERIFIED_RETAIL, MODIFIED, UNKNOWN, NOT_CHECKED)

# The largest datafile read: Redump's biggest lists are a few MiB.
MAX_DAT_BYTES = 64 * 1024 * 1024
MAX_DAT_ROMS = 200_000
# Serials kept per <game>, as the C reader keeps them.
MAX_GAME_SERIALS = 16
_XML_ENTITIES = {"&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": '"', "&apos;": "'"}
HASH_CHUNK_BYTES = 1024 * 1024
# The imported list, relative to the user's data folder (nk_disc_verify.h has the same path).
DAT_RELATIVE_PATH = Path("verification") / "redump.dat"

_GAME = re.compile(r"<game\b[^>]*>(.*?)</game\s*>", re.DOTALL)
_ROM = re.compile(r"<rom\b([^>]*?)/?>")
_ATTRIBUTE = re.compile(r'([A-Za-z_][A-Za-z0-9_.-]*)\s*=\s*"([^"]*)"')
_SERIAL = re.compile(r"<serial>([^<]*)</serial\s*>")
_HEADER_NAME = re.compile(r"<header\b.*?<name>([^<]*)</name\s*>.*?</header\s*>", re.DOTALL)
_HEADER_VERSION = re.compile(r"<header\b.*?<version>([^<]*)</version\s*>.*?</header\s*>",
                             re.DOTALL)
_GAME_NAME = re.compile(r'\bname\s*=\s*"([^"]*)"')
_SHA1 = re.compile(r"[0-9a-fA-F]{40}")
_CRC32 = re.compile(r"[0-9a-fA-F]{8}")
_SIZE = re.compile(r"[0-9]{1,20}")
_RETAIL_DISC_ID = re.compile(r"[A-Z]{4}[0-9]{5}")
_ENTITY = re.compile("|".join(re.escape(entity) for entity in _XML_ENTITIES))


def _xml_text(text: str) -> str:
    """Element or attribute text with XML's five predefined entities decoded, and no others."""
    return _ENTITY.sub(lambda match: _XML_ENTITIES[match.group(0)], text)


class DiscVerifyError(ValueError):
    """A datafile or image that cannot be used, with a named boundary code."""

    def __init__(self, message: str, *, boundary_code: str) -> None:
        super().__init__(message)
        self.boundary_code = boundary_code


@dataclass(frozen=True)
class DiscDigest:
    """The whole-image hashes of one disc image: lowercase hex, and its size in bytes."""

    sha1: str
    crc32: str
    size_bytes: int

    def to_dict(self) -> dict:
        return {"sha1": self.sha1, "crc32": self.crc32, "size_bytes": self.size_bytes}


@dataclass(frozen=True)
class DatRom:
    name: str
    size_bytes: int
    crc32: str
    sha1: str
    serials: tuple[str, ...] = ()


@dataclass
class RedumpDat:
    """The parts of one datafile verification uses."""

    name: str = ""
    version: str = ""
    roms_by_sha1: dict[str, DatRom] = field(default_factory=dict)
    serials: frozenset[str] = frozenset()
    games: int = 0
    rejected_roms: int = 0

    def __len__(self) -> int:
        return len(self.roms_by_sha1)


def normalize_serials(text: str) -> tuple[str, ...]:
    """Disc IDs in a ``<serial>`` value ("TEST-71002, TEST-71002GA") as PARAM.SFO spells them."""
    serials = []
    for part in text.split(","):
        compact = re.sub(r"[\s-]", "", part).upper()
        match = _RETAIL_DISC_ID.match(compact)
        if match and match.group(0) not in serials:
            serials.append(match.group(0))
    return tuple(serials)


def parse_dat_text(text: str) -> RedumpDat:
    """Read a Logiqx-format datafile (Redump's) from its text."""
    if "<datafile" not in text:
        raise DiscVerifyError("This file isn't a Redump datafile: it has no <datafile> element.",
                              boundary_code="DISC_DAT_NOT_DATAFILE")
    dat = RedumpDat()
    header_name = _HEADER_NAME.search(text)
    header_version = _HEADER_VERSION.search(text)
    dat.name = _xml_text(header_name.group(1)).strip() if header_name else ""
    dat.version = _xml_text(header_version.group(1)).strip() if header_version else ""
    serials: set[str] = set()
    for game in _GAME.finditer(text):
        dat.games += 1
        body = game.group(1)
        game_serials: list[str] = []
        for serial in _SERIAL.finditer(body):
            for disc_id in normalize_serials(_xml_text(serial.group(1))):
                if disc_id not in game_serials and len(game_serials) < MAX_GAME_SERIALS:
                    game_serials.append(disc_id)
        serials.update(game_serials)
        for rom in _ROM.finditer(body):
            attributes = dict(_ATTRIBUTE.findall(rom.group(1)))
            sha1 = attributes.get("sha1", "")
            crc = attributes.get("crc", "")
            size = attributes.get("size", "")
            if not (_SHA1.fullmatch(sha1) and _CRC32.fullmatch(crc) and _SIZE.fullmatch(size)
                    and int(size) < 2**64):
                dat.rejected_roms += 1
                continue
            if len(dat.roms_by_sha1) >= MAX_DAT_ROMS:
                raise DiscVerifyError(f"The datafile lists more than {MAX_DAT_ROMS} discs.",
                                      boundary_code="DISC_DAT_TOO_LARGE")
            entry = DatRom(name=_xml_text(attributes.get("name", "")), size_bytes=int(size),
                           crc32=crc.lower(), sha1=sha1.lower(), serials=tuple(game_serials))
            dat.roms_by_sha1.setdefault(entry.sha1, entry)
    dat.serials = frozenset(serials)
    if not dat.roms_by_sha1:
        raise DiscVerifyError("The datafile lists no disc with a SHA-1 hash.",
                              boundary_code="DISC_DAT_EMPTY")
    return dat


def load_dat(path: Path | str) -> RedumpDat:
    """Read a datafile from disk, refusing one over ``MAX_DAT_BYTES``."""
    path = Path(path)
    try:
        with path.open("rb") as handle:
            data = handle.read(MAX_DAT_BYTES + 1)
    except OSError as exc:
        raise DiscVerifyError(f"The datafile can't be read: {exc}",
                              boundary_code="DISC_DAT_READ_FAILED") from exc
    if len(data) > MAX_DAT_BYTES:
        raise DiscVerifyError(f"The datafile is larger than {MAX_DAT_BYTES} bytes.",
                              boundary_code="DISC_DAT_TOO_LARGE")
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise DiscVerifyError("The datafile isn't UTF-8 text.",
                              boundary_code="DISC_DAT_NOT_DATAFILE") from exc
    return parse_dat_text(text)


def imported_dat_path(user_data_root: Path | str) -> Path:
    """Where the player and the tools keep the user's imported datafile."""
    return Path(user_data_root) / DAT_RELATIVE_PATH


def import_dat(source: Path | str, user_data_root: Path | str) -> RedumpDat:
    """Check a datafile the user chose and keep a copy in their data folder.

    The copy replaces any earlier import whole (written beside it, then renamed), so a reader
    never sees half a list. The file it is written to is created new under a random name, so a
    link already in the folder is never written through. Nothing leaves the machine.
    """
    data = Path(source)
    dat = load_dat(data)
    target = imported_dat_path(user_data_root)
    temporary: Path | None = None
    folder = target.parent
    root = Path(user_data_root).resolve(strict=False)
    if (folder.is_symlink() or folder.is_junction() or target.is_symlink()
            or not folder.resolve(strict=False).is_relative_to(root)):
        raise DiscVerifyError("The data folder's verification folder is a link; the list was not "
                              "saved outside the data folder.",
                              boundary_code="DISC_DAT_WRITE_FAILED")
    try:
        folder.mkdir(parents=True, exist_ok=True)
        handle, name = tempfile.mkstemp(dir=folder, prefix=f".{target.name}.", suffix=".tmp")
        temporary = Path(name)
        with os.fdopen(handle, "wb") as stream:
            stream.write(data.read_bytes())
        os.replace(temporary, target)
    except OSError as exc:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise DiscVerifyError(f"The datafile can't be saved in the data folder: {exc}",
                              boundary_code="DISC_DAT_WRITE_FAILED") from exc
    return dat


def load_imported_dat(user_data_root: Path | str) -> Optional[RedumpDat]:
    """The datafile the user imported into this data folder, or None when there is none."""
    path = imported_dat_path(user_data_root)
    return load_dat(path) if path.is_file() else None


def hash_image(path: Path | str, *,
               on_progress: Optional[Callable[[int, int], None]] = None) -> DiscDigest:
    """SHA-1 and CRC32 of a whole plain disc image, read once in 1 MiB chunks."""
    path = Path(path)
    sha1 = hashlib.sha1(usedforsecurity=False)
    crc = 0
    done = 0
    try:
        total = path.stat().st_size
        with path.open("rb") as handle:
            while chunk := handle.read(HASH_CHUNK_BYTES):
                sha1.update(chunk)
                crc = zlib.crc32(chunk, crc)
                done += len(chunk)
                if on_progress is not None:
                    on_progress(done, total)
    except OSError as exc:
        raise DiscVerifyError(f"The disc image can't be read: {exc}",
                              boundary_code="DISC_IMAGE_READ_FAILED") from exc
    return DiscDigest(sha1=sha1.hexdigest(), crc32=f"{crc & 0xFFFFFFFF:08x}", size_bytes=done)


def classify(disc_id: Optional[str], digest: Optional[DiscDigest], dat: Optional[RedumpDat],
             known_retail_ids: Iterable[str] = ()) -> str:
    """The tier of one image; ``known_retail_ids`` are disc IDs of verified images beside it."""
    if dat is None or digest is None:
        return NOT_CHECKED
    rom = dat.roms_by_sha1.get(digest.sha1.lower())
    if rom is not None and rom.crc32 == digest.crc32.lower() and rom.size_bytes == digest.size_bytes:
        return VERIFIED_RETAIL
    disc_id = (disc_id or "").upper()
    if disc_id and (disc_id in dat.serials or disc_id in set(known_retail_ids)):
        return MODIFIED
    return UNKNOWN


def classify_all(images: Mapping[str, tuple[Optional[str], Optional[DiscDigest]]],
                 dat: Optional[RedumpDat]) -> dict[str, str]:
    """Tiers for a whole library at once, so the order images were added never changes one.

    ``images`` maps a caller's key to (disc ID, digest). A disc ID is known retail when any
    image in the set with that disc ID is verified.
    """
    verified_ids = {
        (disc_id or "").upper()
        for disc_id, digest in images.values()
        if disc_id and classify(disc_id, digest, dat) == VERIFIED_RETAIL
    }
    return {key: classify(disc_id, digest, dat, verified_ids)
            for key, (disc_id, digest) in images.items()}
