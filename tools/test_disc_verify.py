# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""nk_core.disc_verify and the bring-up's library records keyed by disc ID plus hash.

Synthetic data only: a datafile in Redump's layout naming made-up images, never a real list.
The C reader (src/core/nk_disc_verify.c) has the same cases in tests/native/test_disc_verify.c.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zlib

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from nk_core import disc_verify  # noqa: E402

RETAIL = bytes((index * 31 + 7) & 0xFF for index in range(8192))
MODIFIED = RETAIL[:4096] + bytes([RETAIL[4096] ^ 0x5A]) + RETAIL[4097:]
OTHER = bytes((index * 17 + 3) & 0xFF for index in range(8192))


def digest(data: bytes) -> disc_verify.DiscDigest:
    return disc_verify.DiscDigest(sha1=hashlib.sha1(data).hexdigest(),
                                  crc32=f"{zlib.crc32(data):08x}", size_bytes=len(data))


def datafile(serials: bool = True) -> str:
    retail = digest(RETAIL)
    serial = "\t\t<serial>TEST-70001, TEST-70001GA</serial>\n" if serials else ""
    return (
        '<?xml version="1.0"?>\n'
        '<!DOCTYPE datafile PUBLIC "-//Logiqx//DTD ROM Management Datafile//EN" '
        '"http://www.logiqx.com/Dats/datafile.dtd">\n'
        "<datafile>\n\t<header>\n\t\t<name>Synthetic &amp; Test</name>\n"
        "\t\t<version>2026-10-31</version>\n\t</header>\n"
        '\t<game name="Retail Disc (USA)">\n\t\t<category>Games</category>\n'
        f"{serial}"
        f'\t\t<rom name="Retail Disc (USA).iso" size="{retail.size_bytes}" crc="{retail.crc32}" '
        f'md5="0" sha1="{retail.sha1}"/>\n\t</game>\n'
        '\t<game name="Bad Row">\n\t\t<rom name="Bad.iso" size="12" crc="zz" sha1="not-a-hash"/>\n'
        "\t</game>\n"
        f'\t<game name="Repeated">\n\t\t<rom name="Again.iso" size="{retail.size_bytes}" '
        f'crc="{retail.crc32}" sha1="{retail.sha1}"/>\n\t</game>\n'
        '\t<game name="Elsewhere (Japan)">\n\t\t<rom name="Elsewhere.iso" size="4" crc="00000001" '
        'sha1="00112233445566778899aabbccddeeff00112233"/>\n\t</game>\n'
        "</datafile>\n"
    )


class DatafileTests(unittest.TestCase):
    def test_reader_takes_hashes_sizes_and_serials(self) -> None:
        dat = disc_verify.parse_dat_text(datafile())
        self.assertEqual((dat.name, dat.version), ("Synthetic & Test", "2026-10-31"))
        self.assertEqual((dat.games, dat.rejected_roms, len(dat)), (4, 1, 2))
        self.assertEqual(dat.serials, frozenset({"TEST70001"}))
        rom = dat.roms_by_sha1[digest(RETAIL).sha1]
        self.assertEqual((rom.name, rom.size_bytes, rom.serials),
                         ("Retail Disc (USA).iso", 8192, ("TEST70001",)))

    def test_the_reader_follows_the_c_reader_on_edge_cases(self) -> None:
        retail = digest(RETAIL)
        rom = f'<rom name="A &amp; B &#38; C" size="{{size}}" crc="{retail.crc32}" sha1="{retail.sha1}"/>'
        many = ", ".join(f"TEST-7{index:04d}" for index in range(20))
        text = (f"<datafile><game name=\"x\"><serial>{many}</serial>"
                f"<serial>TEST&#45;79999</serial>{rom.format(size=len(RETAIL))}</game>"
                f"<game name=\"y\">{rom.format(size=2**64).replace(retail.sha1, 'f' * 40)}"
                "</game></datafile>")
        dat = disc_verify.parse_dat_text(text)
        # Only the five predefined entities are decoded, as in C.
        self.assertEqual(dat.roms_by_sha1[retail.sha1].name, "A & B &#38; C")
        # At most 16 serials per game, and a numeric entity is not decoded into one.
        self.assertEqual(len(dat.serials), 16)
        self.assertNotIn("TEST79999", dat.serials)
        # A size past 64 bits is a malformed row.
        self.assertEqual((len(dat), dat.rejected_roms), (1, 1))

    def test_serials_normalize_to_the_disc_id_param_sfo_holds(self) -> None:
        self.assertEqual(
            disc_verify.normalize_serials("TEST-71002, TEST-71002GA, test 71003,BAD,TEST-7100"),
            ("TEST71002", "TEST71003"))

    def test_refusals_are_named(self) -> None:
        with self.assertRaises(disc_verify.DiscVerifyError) as raised:
            disc_verify.parse_dat_text("<html>nothing</html>")
        self.assertEqual(raised.exception.boundary_code, "DISC_DAT_NOT_DATAFILE")
        with self.assertRaises(disc_verify.DiscVerifyError) as raised:
            disc_verify.parse_dat_text('<datafile><game name="x"><rom size="1"/></game></datafile>')
        self.assertEqual(raised.exception.boundary_code, "DISC_DAT_EMPTY")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "big.dat"
            path.write_text(datafile(), encoding="utf-8")
            with mock.patch.object(disc_verify, "MAX_DAT_BYTES", 64):
                with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                    disc_verify.load_dat(path)
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_TOO_LARGE")
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.load_dat(Path(folder) / "missing.dat")
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_READ_FAILED")


class TierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dat = disc_verify.parse_dat_text(datafile())

    def test_verified_modified_unknown_and_not_checked(self) -> None:
        classify = disc_verify.classify
        self.assertEqual(classify("TEST70001", digest(RETAIL), self.dat),
                         disc_verify.VERIFIED_RETAIL)
        self.assertEqual(classify("TEST70001", digest(MODIFIED), self.dat), disc_verify.MODIFIED)
        self.assertEqual(classify("TEST70009", digest(OTHER), self.dat), disc_verify.UNKNOWN)
        self.assertEqual(classify("TEST70009", digest(OTHER), self.dat, {"TEST70009"}),
                         disc_verify.MODIFIED)
        self.assertEqual(classify("TEST70001", digest(RETAIL), None), disc_verify.NOT_CHECKED)
        self.assertEqual(classify("TEST70001", None, self.dat), disc_verify.NOT_CHECKED)

    def test_a_matching_sha1_with_another_crc_or_size_is_not_verified(self) -> None:
        good = digest(RETAIL)
        bent_crc = disc_verify.DiscDigest(good.sha1, "00000000", good.size_bytes)
        bent_size = disc_verify.DiscDigest(good.sha1, good.crc32, good.size_bytes + 2048)
        for bent in (bent_crc, bent_size):
            self.assertEqual(disc_verify.classify("TEST70009", bent, self.dat),
                             disc_verify.UNKNOWN)

    def test_a_serial_less_list_learns_retail_ids_from_verified_images(self) -> None:
        dat = disc_verify.parse_dat_text(datafile(serials=False))
        images = {"translation": ("TEST70001", digest(MODIFIED)),
                  "retail": ("TEST70001", digest(RETAIL)),
                  "other": ("TEST70009", digest(OTHER)),
                  "unread": ("TEST70002", None)}
        expected = {"translation": disc_verify.MODIFIED, "retail": disc_verify.VERIFIED_RETAIL,
                    "other": disc_verify.UNKNOWN, "unread": disc_verify.NOT_CHECKED}
        self.assertEqual(disc_verify.classify_all(images, dat), expected)
        # The order images arrive in never changes a tier.
        self.assertEqual(disc_verify.classify_all(dict(reversed(images.items())), dat), expected)
        self.assertEqual(disc_verify.classify("TEST70001", digest(MODIFIED), dat),
                         disc_verify.UNKNOWN)


class HashAndImportTests(unittest.TestCase):
    def test_whole_image_hash_matches_sha1_and_crc32(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / "disc.iso"
            data = RETAIL * 300  # several read chunks, not a whole number of them
            image.write_bytes(data)
            seen: list[tuple[int, int]] = []
            result = disc_verify.hash_image(image, on_progress=lambda done, total:
                                            seen.append((done, total)))
            self.assertEqual(result, digest(data))
            self.assertEqual(seen[-1], (len(data), len(data)))
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.hash_image(Path(folder) / "missing.iso")
            self.assertEqual(raised.exception.boundary_code, "DISC_IMAGE_READ_FAILED")
        # The CRC-32 check value, as the C reader's test checks it.
        self.assertEqual(f"{zlib.crc32(b'123456789'):08x}", "cbf43926")

    def test_import_keeps_a_whole_copy_in_the_data_folder(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "chosen.dat"
            source.write_text(datafile(), encoding="utf-8")
            data = root / "data"
            self.assertIsNone(disc_verify.load_imported_dat(data))
            self.assertEqual(len(disc_verify.import_dat(source, data)), 2)
            imported = data / "verification" / "redump.dat"
            self.assertEqual(imported.read_bytes(), source.read_bytes())
            bad = root / "bad.dat"
            bad.write_text("hello", encoding="utf-8")
            with self.assertRaises(disc_verify.DiscVerifyError):
                disc_verify.import_dat(bad, data)
            self.assertEqual(imported.read_bytes(), source.read_bytes())
            source.write_text(datafile(serials=False), encoding="utf-8")
            disc_verify.import_dat(source, data)
            # A verification folder that links elsewhere is refused; the file there is kept.
            outside = root / "outside"
            outside.mkdir()
            (outside / "redump.dat").write_text("kept", encoding="utf-8")
            linked = root / "linked"
            linked.mkdir()
            try:
                (linked / "verification").symlink_to(outside, target_is_directory=True)
            except OSError:
                pass  # this host cannot make a symlink without privilege
            else:
                with self.assertRaises(disc_verify.DiscVerifyError):
                    disc_verify.import_dat(source, linked)
                self.assertEqual((outside / "redump.dat").read_text(encoding="utf-8"), "kept")
                # Nor is a link left where an earlier import named its temporary file.
                planted = data / "verification" / f".redump.dat.{os.getpid()}.tmp"
                planted.symlink_to(outside / "redump.dat")
                disc_verify.import_dat(source, data)
                self.assertEqual((outside / "redump.dat").read_text(encoding="utf-8"), "kept")
                self.assertTrue(planted.is_symlink())
                planted.unlink()
            self.assertEqual(disc_verify.load_imported_dat(data).serials, frozenset())
            self.assertEqual(sorted(path.name for path in imported.parent.iterdir()),
                             ["redump.dat"])


class BringupLibraryTests(unittest.TestCase):
    """nk_cli's library write: the same rules as src/core/nk_library.c."""

    def setUp(self) -> None:
        import nk_cli

        self.nk_cli = nk_cli
        self._tmp = tempfile.TemporaryDirectory(prefix="disc-verify-library-")
        self.root = Path(self._tmp.name)
        (self.root / "chosen.dat").write_text(datafile(serials=False), encoding="utf-8")
        disc_verify.import_dat(self.root / "chosen.dat", self.root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, iso: str, data: bytes | None, disc_id: str = "TEST70001", *,
              hashed: bool = True) -> None:
        """A bring-up adding the image `data` at `iso`; hashed=False is one whose hash failed
        (or a record an older Nakagawa wrote, which never hashed)."""
        path = self.root / iso
        if data is not None:
            path.write_bytes(data)
        metadata = mock.Mock(disc_id=disc_id, title=f"Title {iso}", version="1.00")
        self.nk_cli._write_bringup_library_locked(
            self.root, path, metadata, f"{disc_id}-id", "EBOOT.BIN", True, "",
            digest(path.read_bytes()) if hashed else None)

    def games(self) -> list[dict]:
        return json.loads((self.root / "library.json").read_text(encoding="utf-8"))["games"]

    def by_file(self) -> dict[str, dict]:
        return {Path(game["iso_path"]).name: game for game in self.games()}

    def test_a_fan_translation_and_its_retail_disc_are_both_kept(self) -> None:
        self.write("translation.iso", MODIFIED)
        self.assertEqual([game["disc_verification"] for game in self.games()],
                         [disc_verify.UNKNOWN])
        self.write("retail.iso", RETAIL)
        games = self.by_file()
        self.assertEqual(sorted(games), ["retail.iso", "translation.iso"])
        self.assertEqual(games["retail.iso"]["disc_verification"], disc_verify.VERIFIED_RETAIL)
        self.assertEqual(games["translation.iso"]["disc_verification"], disc_verify.MODIFIED)
        self.assertEqual(games["retail.iso"]["content_sha1"], digest(RETAIL).sha1)
        self.assertEqual(games["retail.iso"]["content_crc32"], digest(RETAIL).crc32)
        self.assertEqual(games["retail.iso"]["iso_size_bytes"], len(RETAIL))
        # The same image again replaces its own record only.
        self.write("retail.iso", RETAIL)
        self.assertEqual(len(self.games()), 2)
        # The build picks the record of the image it was given, and refuses to guess.
        entry = self.nk_cli._load_library_entry(self.root, "TEST70001",
                                                self.root / "translation.iso")
        self.assertEqual(entry["content_sha1"], digest(MODIFIED).sha1)
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "--iso-path"):
            self.nk_cli._load_library_entry(self.root, "TEST70001")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "--iso-path"):
            self.nk_cli._load_library_entry(self.root, "TEST70001", self.root / "other.iso")

    def test_a_library_built_before_hashing_keeps_its_retail_record(self) -> None:
        # Resuming an older build: the retail record has no hash, then its fan translation
        # (once a duplicate) is built. The retail file is hashed now and both are kept.
        self.write("retail.iso", RETAIL, hashed=False)
        self.assertNotIn("content_sha1", self.games()[0])
        self.write("translation.iso", MODIFIED)
        games = self.by_file()
        self.assertEqual(sorted(games), ["retail.iso", "translation.iso"])
        self.assertEqual(games["retail.iso"]["content_sha1"], digest(RETAIL).sha1)
        self.assertEqual(games["retail.iso"]["disc_verification"], disc_verify.VERIFIED_RETAIL)
        self.assertEqual(games["translation.iso"]["disc_verification"], disc_verify.MODIFIED)

    def test_an_older_record_of_the_same_image_elsewhere_is_replaced(self) -> None:
        self.write("old.iso", RETAIL, hashed=False)
        self.write("copy.iso", RETAIL)
        self.assertEqual(sorted(self.by_file()), ["copy.iso"])

    def test_an_older_record_whose_file_is_gone_is_kept(self) -> None:
        self.write("gone.iso", RETAIL, hashed=False)
        (self.root / "gone.iso").unlink()
        self.write("new.iso", OTHER)
        self.assertEqual(sorted(self.by_file()), ["gone.iso", "new.iso"])

    def test_a_failed_hash_replaces_one_record_at_most(self) -> None:
        self.write("retail.iso", RETAIL)
        self.write("translation.iso", MODIFIED)
        # Its own file again, unhashed this time: that record only.
        self.write("translation.iso", MODIFIED, hashed=False)
        games = self.by_file()
        self.assertEqual(sorted(games), ["retail.iso", "translation.iso"])
        self.assertEqual(games["retail.iso"]["content_sha1"], digest(RETAIL).sha1)
        self.assertNotIn("content_sha1", games["translation.iso"])
        # A new file, unhashed, beside two images of its disc ID: added, nothing dropped.
        self.write("third.iso", OTHER, hashed=False)
        self.assertEqual(sorted(self.by_file()), ["retail.iso", "third.iso", "translation.iso"])

    def test_a_failed_hash_never_replaces_a_hashed_record_of_another_file(self) -> None:
        self.write("retail.iso", RETAIL)
        self.write("unread.iso", MODIFIED, hashed=False)
        self.assertEqual(sorted(self.by_file()), ["retail.iso", "unread.iso"])
        # Beside a record that was never hashed either, it replaces it, as before hashing.
        self.write("old.iso", OTHER, disc_id="TEST70009", hashed=False)
        self.write("moved.iso", OTHER, disc_id="TEST70009", hashed=False)
        self.assertNotIn("old.iso", self.by_file())

    def test_the_file_another_image_now_fills_is_replaced(self) -> None:
        self.write("disc.iso", RETAIL)
        self.write("disc.iso", OTHER)  # e.g. a compressed image expanded over the same path
        self.assertEqual([game["content_sha1"] for game in self.games()], [digest(OTHER).sha1])


if __name__ == "__main__":
    unittest.main()
