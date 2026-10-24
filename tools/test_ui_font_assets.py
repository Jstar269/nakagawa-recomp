# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pin the bundled project UI fonts, their static weights, and family OFL texts."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ROOT / "assets/fonts/ui/manifest.json"
UPSTREAM_COMMIT = "45e3d5269e4a17d823f6d84470bc084dc5ae46f6"

EXPECTED = {
    "assets/fonts/ui/chakra-petch/ChakraPetch-Bold.ttf": {
        "family": "Chakra Petch", "weight": 700,
        "sha256": "65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0",
        "upstream_path": "ofl/chakrapetch/ChakraPetch-Bold.ttf",
        "upstream_sha256": "65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0",
        "source_kind": "upstream-static",
        "license_file": "assets/fonts/ui/chakra-petch/OFL.txt",
        "license_sha256": "4a1d840eb0b2df8553804d844960a15773ca5af5d5c9803745307db8c617540b",
        "license_upstream_sha256": "13831d02389d917d22fcfa6c79f98f8acbf61d230add0e73a4d4dc5f3bfb9e56",
    },
    "assets/fonts/ui/chakra-petch/ChakraPetch-SemiBold.ttf": {
        "family": "Chakra Petch", "weight": 600,
        "sha256": "45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b",
        "upstream_path": "ofl/chakrapetch/ChakraPetch-SemiBold.ttf",
        "upstream_sha256": "45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b",
        "source_kind": "upstream-static",
        "license_file": "assets/fonts/ui/chakra-petch/OFL.txt",
        "license_sha256": "4a1d840eb0b2df8553804d844960a15773ca5af5d5c9803745307db8c617540b",
        "license_upstream_sha256": "13831d02389d917d22fcfa6c79f98f8acbf61d230add0e73a4d4dc5f3bfb9e56",
    },
    "assets/fonts/ui/geist/Geist-Regular.ttf": {
        "family": "Geist", "weight": 400,
        "sha256": "58a4bac9a7b901313822aada65bb1469b3c873874e11b3fc88ba75ed9576d98a",
        "upstream_path": "ofl/geist/Geist[wght].ttf",
        "upstream_sha256": "73894e0448cae90a92b6c2f8732b7bb9acb7b94c418bff559dad4a18e1de9659",
        "source_kind": "variable-instance",
        "license_file": "assets/fonts/ui/geist/OFL.txt",
        "license_sha256": "f12f59163d92084b0473315ae4566cde0c1233d940ae464f5225a69c347cc691",
        "license_upstream_sha256": "1781d2806a07d91c4edf4740b88449fab7d0eadad53f7c351b94cd4d4eb8c00f",
        "instance": {"wght": 400},
    },
    "assets/fonts/ui/geist/Geist-Medium.ttf": {
        "family": "Geist", "weight": 500,
        "sha256": "35089cbe873bf2764d4ba9810b90f6b93eee45ea37efe4cd83997825b97ab0db",
        "upstream_path": "ofl/geist/Geist[wght].ttf",
        "upstream_sha256": "73894e0448cae90a92b6c2f8732b7bb9acb7b94c418bff559dad4a18e1de9659",
        "source_kind": "variable-instance",
        "license_file": "assets/fonts/ui/geist/OFL.txt",
        "license_sha256": "f12f59163d92084b0473315ae4566cde0c1233d940ae464f5225a69c347cc691",
        "license_upstream_sha256": "1781d2806a07d91c4edf4740b88449fab7d0eadad53f7c351b94cd4d4eb8c00f",
        "instance": {"wght": 500},
    },
    "assets/fonts/ui/geist/Geist-SemiBold.ttf": {
        "family": "Geist", "weight": 600,
        "sha256": "c157d36cc1778633d43f5a0ff92b078bb6de26f5011a6a092b7b6f49fe022fc5",
        "upstream_path": "ofl/geist/Geist[wght].ttf",
        "upstream_sha256": "73894e0448cae90a92b6c2f8732b7bb9acb7b94c418bff559dad4a18e1de9659",
        "source_kind": "variable-instance",
        "license_file": "assets/fonts/ui/geist/OFL.txt",
        "license_sha256": "f12f59163d92084b0473315ae4566cde0c1233d940ae464f5225a69c347cc691",
        "license_upstream_sha256": "1781d2806a07d91c4edf4740b88449fab7d0eadad53f7c351b94cd4d4eb8c00f",
        "instance": {"wght": 600},
    },
    "assets/fonts/ui/geist-mono/GeistMono-Regular.ttf": {
        "family": "Geist Mono", "weight": 400,
        "sha256": "341cdaf235d2cdcfa85ff4eed9628381a89422bd3439710c80e45b725db70b2c",
        "upstream_path": "ofl/geistmono/GeistMono[wght].ttf",
        "upstream_sha256": "d00e590b8eb3a59acc329b2d044fd143ae935090b7da33199ebee27cc7de8196",
        "source_kind": "variable-instance",
        "license_file": "assets/fonts/ui/geist-mono/OFL.txt",
        "license_sha256": "f12f59163d92084b0473315ae4566cde0c1233d940ae464f5225a69c347cc691",
        "license_upstream_sha256": "1781d2806a07d91c4edf4740b88449fab7d0eadad53f7c351b94cd4d4eb8c00f",
        "instance": {"wght": 400},
    },
    "assets/fonts/ui/geist-mono/GeistMono-Medium.ttf": {
        "family": "Geist Mono", "weight": 500,
        "sha256": "3fc64e39b238a4d4370a35b5226103ab34d04fe741d93bbd59929cab8bbe3500",
        "upstream_path": "ofl/geistmono/GeistMono[wght].ttf",
        "upstream_sha256": "d00e590b8eb3a59acc329b2d044fd143ae935090b7da33199ebee27cc7de8196",
        "source_kind": "variable-instance",
        "license_file": "assets/fonts/ui/geist-mono/OFL.txt",
        "license_sha256": "f12f59163d92084b0473315ae4566cde0c1233d940ae464f5225a69c347cc691",
        "license_upstream_sha256": "1781d2806a07d91c4edf4740b88449fab7d0eadad53f7c351b94cd4d4eb8c00f",
        "instance": {"wght": 500},
    },
    "assets/fonts/ui/noto-sans-jp/NotoSansJP-Regular.ttf": {
        "family": "Noto Sans JP", "weight": 400,
        "sha256": "a66655d7b24ac6f793f73914c60ad44522b9fbd9e5f9b3706220b66c44fdaad2",
        "upstream_path": "ofl/notosansjp/NotoSansJP[wght].ttf",
        "upstream_sha256": "c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f",
        "source_kind": "variable-instance",
        "license_file": "assets/fonts/ui/noto-sans-jp/OFL.txt",
        "license_sha256": "babcfe66c8a098b2fa279bc724a3a342f8124f77ce18941fbcc1bbb39823cded",
        "license_upstream_sha256": "1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9",
        "instance": {"wght": 400},
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sfnt_table_tags(path: Path) -> set[bytes]:
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError(f"truncated SFNT: {path}")
    table_count = struct.unpack_from(">H", data, 4)[0]
    directory_end = 12 + table_count * 16
    if directory_end > len(data):
        raise ValueError(f"truncated SFNT table directory: {path}")
    return {data[12 + i * 16 : 16 + i * 16] for i in range(table_count)}


class UiFontAssetsTests(unittest.TestCase):
    def test_manifest_pins_exact_static_fonts_and_adjacent_ofl_text(self) -> None:
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["upstream"]["repository"], "https://github.com/google/fonts")
        self.assertEqual(manifest["upstream"]["commit"], UPSTREAM_COMMIT)
        self.assertEqual(
            manifest["static_instance_tool"],
            {
                "name": "fontTools varLib.instancer",
                "version": "4.67.0",
                "arguments": ["--static", "--update-name-table", "--no-recalc-timestamp", "wght=<weight>"],
            },
        )
        entries = manifest["fonts"]
        self.assertEqual(len(entries), len(EXPECTED))
        self.assertEqual({entry["file"] for entry in entries}, set(EXPECTED))

        for entry in entries:
            path = entry["file"]
            expected = EXPECTED[path]
            with self.subTest(font=path):
                for key, value in expected.items():
                    self.assertEqual(entry.get(key), value, f"manifest {key} drifted for {path}")
                self.assertTrue(entry["static"], f"{path} must be a static TTF")
                font_path = ROOT / path
                self.assertTrue(font_path.is_file(), f"font is missing: {path}")
                self.assertEqual(font_path.stat().st_size, entry["size_bytes"], path)
                self.assertEqual(sha256(font_path), entry["sha256"], f"font hash mismatch: {path}")
                tags = sfnt_table_tags(font_path)
                self.assertIn(b"glyf", tags, f"expected TrueType outlines: {path}")
                self.assertNotIn(b"fvar", tags, f"variable font cannot select a fixed weight: {path}")

                license_path = ROOT / entry["license_file"]
                self.assertTrue(license_path.is_file(), f"OFL text is missing: {entry['license_file']}")
                self.assertEqual(entry["license_spdx"], "OFL-1.1")
                self.assertEqual(
                    sha256(license_path), entry["license_sha256"],
                    f"OFL text hash mismatch: {entry['license_file']}",
                )
                self.assertIn(
                    b"SIL OPEN FONT LICENSE Version 1.1", license_path.read_bytes(),
                    f"OFL text is invalid: {entry['license_file']}",
                )


if __name__ == "__main__":
    unittest.main()
